import random
import string
import pytest
import asyncio
import time
import socket
import random
import ssl
from redis import asyncio as aioredis
from redis.exceptions import ConnectionError as redis_conn_error, ResponseError
import async_timeout
from dataclasses import dataclass
from aiohttp import ClientSession

from . import dfly_args
from .instance import DflyInstance, DflyInstanceFactory

BASE_PORT = 1111


@dataclass(frozen=True)
class CollectedRedisMsg:
    cmd: str
    src: str = "tcp"

    @staticmethod
    def all_from_src(*args, src="tcp"):
        return [CollectedRedisMsg(arg, src) for arg in args]


class CollectingMonitor:
    """Tracks all monitor messages between start() and stop()"""

    def __init__(self, client):
        self.client = client
        self.messages = []
        self._monitor_task = None

    async def _monitor(self):
        async with self.client.monitor() as monitor:
            async for message in monitor.listen():
                self.messages.append(CollectedRedisMsg(message["command"], message["client_type"]))

    async def start(self):
        if self._monitor_task is None:
            self._monitor_task = asyncio.create_task(self._monitor())
        await asyncio.sleep(0.1)

    async def stop(self, timeout=0.1):
        if self._monitor_task:
            # Wait for Dragonfly to send all async monitor messages
            await asyncio.sleep(timeout)
            self._monitor_task.cancel()
            try:
                await self._monitor_task
            except asyncio.CancelledError:
                pass
            self._monitor_task = None

        def should_exclude(cmd: str):
            cmd = cmd.upper()
            return "SELECT" in cmd or "CLIENT SETINFO" in cmd

        while len(self.messages) > 0 and should_exclude(self.messages[0].cmd):
            self.messages = self.messages[1:]
        return self.messages


"""
Test MONITOR command with basic use case
"""


@dfly_args({"proactor_threads": 4})
async def test_monitor_command(async_pool):
    monitor = CollectingMonitor(aioredis.Redis(connection_pool=async_pool))
    await monitor.start()

    c = aioredis.Redis(connection_pool=async_pool)
    await c.set("a", 1)
    await c.get("a")
    await c.lpush("l", "V")
    await c.lpop("l")

    collected = await monitor.stop()
    expected = CollectedRedisMsg.all_from_src("SET a 1", "GET a", "LPUSH l V", "LPOP l")

    assert expected == collected


"""
Test MONITOR command with MULTI/EXEC transaction with squashing
"""


@dfly_args({"proactor_threads": 4, "multi_exec_squash": "true"})
async def test_monitor_command_multi(async_pool):
    monitor = CollectingMonitor(aioredis.Redis(connection_pool=async_pool))
    await monitor.start()

    c = aioredis.Redis(connection_pool=async_pool)
    p = c.pipeline(transaction=True)

    expected = []
    for i in range(100):
        p.lpush(str(i), "V")
        expected.append(f"LPUSH {i} V")

    await p.execute()

    collected = await monitor.stop(0.3)
    expected = CollectedRedisMsg.all_from_src(*expected)

    # The order is random due to squashing
    assert set(expected) == set(collected[2:])


"""
Test MONITOR command with lua script
https://github.com/dragonflydb/dragonfly/issues/756
"""

TEST_MONITOR_SCRIPT = """
    redis.call('SET', 'A', 1)
    redis.call('GET', 'A')
    redis.call('SADD', 'S', 1, 2, 3)
    redis.call('LPUSH', 'L', 1)
    redis.call('LPOP', 'L')
"""


@dfly_args({"proactor_threads": 4, "lua_auto_async": "false"})
async def test_monitor_command_lua(async_pool):
    monitor = CollectingMonitor(aioredis.Redis(connection_pool=async_pool))
    await monitor.start()

    c = aioredis.Redis(connection_pool=async_pool)
    await c.eval(TEST_MONITOR_SCRIPT, 3, "A", "S", "L")

    collected = await monitor.stop()
    expected = CollectedRedisMsg.all_from_src(
        "SET A 1", "GET A", "SADD S 1 2 3", "LPUSH L 1", "LPOP L", src="lua"
    )

    assert expected == collected[1:]


"""
Run test in pipeline mode.
This is mostly how this is done with python - its more like a transaction that
the connections is running all commands in its context
"""


async def test_pipeline_support(async_client):
    def generate(max):
        for i in range(max):
            yield f"key{i}", f"value={i}"

    messages = {a: b for a, b in generate(5)}
    assert await run_pipeline_mode(async_client, messages)


async def reader(channel: aioredis.client.PubSub, messages, max: int):
    message_count = len(messages)
    while message_count > 0:
        try:
            async with async_timeout.timeout(1):
                message = await channel.get_message(ignore_subscribe_messages=True)
                if message is not None:
                    message_count = message_count - 1
                    if message["data"] not in messages:
                        return False, f"got unexpected message from pubsub - {message['data']}"
                await asyncio.sleep(0.01)
        except asyncio.TimeoutError:
            pass
    return True, "success"


async def run_pipeline_mode(async_client: aioredis.Redis, messages):
    pipe = async_client.pipeline(transaction=False)
    for key, val in messages.items():
        pipe.set(key, val)
    result = await pipe.execute()

    print(f"got result from the pipeline of {result} with len = {len(result)}")
    if len(result) != len(messages):
        return False, f"number of results from pipe {len(result)} != expected {len(messages)}"
    elif False in result:
        return False, "expecting to successfully get all result good, but some failed"
    else:
        return True, "all command processed successfully"


"""
Test the pipeline command
Open connection to the subscriber and publish on the other end messages
Make sure that we are able to send all of them and that we are getting the
expected results on the subscriber side
"""


async def test_pubsub_command(async_client):
    def generate(max):
        for i in range(max):
            yield f"message number {i}"

    messages = [a for a in generate(5)]
    assert await run_pubsub(async_client, messages, "channel-1")


async def run_pubsub(async_client, messages, channel_name):
    pubsub = async_client.pubsub()
    await pubsub.subscribe(channel_name)

    future = asyncio.create_task(reader(pubsub, messages, len(messages)))
    success = True

    for message in messages:
        res = await async_client.publish(channel_name, message)
        if not res:
            success = False
            break

    await future
    status, message = future.result()

    await pubsub.close()
    if status and success:
        return True, "successfully completed all"
    else:
        return (
            False,
            f"subscriber result: {status}: {message},  publisher publish: success {success}",
        )


async def run_multi_pubsub(async_client, messages, channel_name):
    subs = [async_client.pubsub() for i in range(5)]
    for s in subs:
        await s.subscribe(channel_name)

    tasks = [
        asyncio.create_task(reader(s, messages, random.randint(0, len(messages)))) for s in subs
    ]

    success = True

    for message in messages:
        res = await async_client.publish(channel_name, message)
        if not res:
            success = False
            break

    for f in tasks:
        await f
    results = [f.result() for f in tasks]

    for s in subs:
        await s.close()
    if success:
        for status, message in results:
            if not status:
                return False, f"failed to process {message}"
        return True, "success"
    else:
        return False, "failed to publish"


"""
Test with multiple subscribers for a channel
We want to stress this to see if we have any issue
with the pub sub code since we are "sharing" the message
across multiple connections internally
"""


async def test_multi_pubsub(async_client):
    def generate(max):
        for i in range(max):
            yield f"this is message number {i} from the publisher on the channel"

    messages = [a for a in generate(500)]
    state, message = await run_multi_pubsub(async_client, messages, "my-channel")

    assert state, message


"""
Test PUBSUB NUMSUB command.
"""


async def test_pubsub_subcommand_for_numsub(async_client: aioredis.Redis):
    async def resub(s: "aioredis.PubSub", sub: bool, chan: str):
        if sub:
            await s.subscribe(chan)
        else:
            await s.unsubscribe(chan)
        # Wait for PUSH message to be parsed to make sure upadte was performed
        await s.get_message(timeout=0.1)

    # Subscribe 5 times to chan1
    subs1 = [async_client.pubsub() for i in range(5)]
    await asyncio.gather(*(resub(s, True, "chan1") for s in subs1))
    assert await async_client.pubsub_numsub("chan1") == [("chan1", 5)]

    # Unsubscribe all from chan1
    await asyncio.gather(*(resub(s, False, "chan1") for s in subs1))

    # Make sure numsub drops to 0
    with async_timeout.timeout(1):
        while (await async_client.pubsub_numsub("chan1"))[0][1] > 0:
            await asyncio.sleep(0.05)

    # Check empty numsub
    assert await async_client.pubsub_numsub() == []

    subs2 = [async_client.pubsub() for i in range(5)]
    await asyncio.gather(*(resub(s, True, "chan2") for s in subs2))

    subs3 = [async_client.pubsub() for i in range(10)]
    await asyncio.gather(*(resub(s, True, "chan3") for s in subs3))

    assert await async_client.pubsub_numsub("chan2", "chan3") == [("chan2", 5), ("chan3", 10)]

    await asyncio.gather(*(s.unsubscribe() for s in subs2 + subs3))


"""
Test that pubsub clients who are stuck on backpressure from a slow client (the one in the test doesn't read messages at all)
will eventually unblock when it disconnects.
"""


@pytest.mark.slow
@dfly_args({"proactor_threads": "1", "subscriber_thread_limit": "100"})
async def test_publish_stuck(df_server: DflyInstance, async_client: aioredis.Redis):
    reader, writer = await asyncio.open_connection("127.0.0.1", df_server.port, limit=10)
    writer.write(b"SUBSCRIBE channel\r\n")
    await writer.drain()

    async def pub_task():
        payload = "msg" * 1000
        p = async_client.pipeline()
        for _ in range(1000):
            p.publish("channel", payload)
        await p.execute()

    publishers = [asyncio.create_task(pub_task()) for _ in range(20)]

    await asyncio.sleep(5)

    # Check we reached the limit
    pub_bytes = int((await async_client.info())["dispatch_queue_subscriber_bytes"])
    assert pub_bytes >= 100

    await asyncio.sleep(0.1)

    # Make sure processing is stalled
    new_pub_bytes = int((await async_client.info())["dispatch_queue_subscriber_bytes"])
    assert new_pub_bytes == pub_bytes

    writer.write(b"QUIT\r\n")
    await writer.drain()
    writer.close()

    # Make sure all publishers unblock eventually
    for pub in asyncio.as_completed(publishers):
        await pub


async def test_subscribers_with_active_publisher(df_server: DflyInstance, max_connections=100):
    # TODO: I am not how to customize the max connections for the pool.
    async_pool = aioredis.ConnectionPool(
        host="localhost",
        port=df_server.port,
        db=0,
        decode_responses=True,
        max_connections=max_connections,
    )

    async def publish_worker():
        client = aioredis.Redis(connection_pool=async_pool)
        for i in range(0, 2000):
            await client.publish("channel", f"message-{i}")
        await client.close()

    async def channel_reader(channel: aioredis.client.PubSub):
        for i in range(0, 150):
            try:
                async with async_timeout.timeout(1):
                    message = await channel.get_message(ignore_subscribe_messages=True)
            except asyncio.TimeoutError:
                break

    async def subscribe_worker():
        client = aioredis.Redis(connection_pool=async_pool)
        pubsub = client.pubsub()
        async with pubsub as p:
            await pubsub.subscribe("channel")
            await channel_reader(pubsub)
            await pubsub.unsubscribe("channel")

    # Create a publisher that sends constantly messages to the channel
    # Then create subscribers that will subscribe to already active channel
    pub_task = asyncio.create_task(publish_worker())
    await asyncio.gather(*(subscribe_worker() for _ in range(max_connections - 10)))
    await pub_task
    await async_pool.disconnect()


async def test_big_command(df_server, size=8 * 1024):
    reader, writer = await asyncio.open_connection("127.0.0.1", df_server.port)

    writer.write(f"SET a {'v'*size}\n".encode())
    await writer.drain()

    assert "OK" in (await reader.readline()).decode()

    writer.close()
    await writer.wait_closed()


async def test_subscribe_pipelined(async_client: aioredis.Redis):
    pipe = async_client.pipeline(transaction=False)
    pipe.execute_command("subscribe channel").execute_command("subscribe channel")
    await pipe.echo("bye bye").execute()


async def test_subscribe_in_pipeline(async_client: aioredis.Redis):
    pipe = async_client.pipeline(transaction=False)
    pipe.echo("one")
    pipe.execute_command("SUBSCRIBE ch1")
    pipe.echo("two")
    pipe.execute_command("SUBSCRIBE ch2")
    pipe.echo("three")
    res = await pipe.execute()

    assert res == ["one", ["subscribe", "ch1", 1], "two", ["subscribe", "ch2", 2], "three"]


"""
This test makes sure that Dragonfly can receive blocks of pipelined commands even
while a script is still executing. This is a dangerous scenario because both the dispatch fiber
and the connection fiber are actively using the context. What is more, the script execution injects
its own custom reply builder, which can't be used anywhere else, besides the lua script itself.
"""

BUSY_SCRIPT = """
for i=1,300 do
    redis.call('MGET', 'k1', 'k2', 'k3')
end
"""

PACKET1 = """
MGET s1 s2 s3
EVALSHA {sha} 3 k1 k2 k3
"""

PACKET2 = """
MGET m1 m2 m3
MGET m4 m5 m6
MGET m7 m8 m9\n
"""

PACKET3 = (
    """
PING
"""
    * 500
    + "ECHO DONE\n"
)


async def test_parser_while_script_running(async_client: aioredis.Redis, df_server: DflyInstance):
    sha = await async_client.script_load(BUSY_SCRIPT)

    # Use a raw tcp connection for strict control of sent commands
    # Below we send commands while the previous ones didn't finish
    reader, writer = await asyncio.open_connection("localhost", df_server.port)

    # Send first pipeline packet, last commands is a long executing script
    writer.write(PACKET1.format(sha=sha).encode())
    await writer.drain()

    # Give the script some time to start running
    await asyncio.sleep(0.01)

    # Send another packet that will be received while the script is running
    writer.write(PACKET2.encode())
    # The last batch has to be big enough, so the script will finish before it is fully consumed
    writer.write(PACKET3.encode())
    await writer.drain()

    await reader.readuntil(b"DONE")
    writer.close()
    await writer.wait_closed()


"""
    This test makes sure that we can migrate while handling pipelined commands and don't keep replies
    batched even if the stream suddenly stops.
"""


@dfly_args({"proactor_threads": "4", "pipeline_squash": 0})
async def test_pipeline_batching_while_migrating(
    async_client: aioredis.Redis, df_server: DflyInstance
):
    sha = await async_client.script_load("return redis.call('GET', KEYS[1])")

    reader, writer = await asyncio.open_connection("localhost", df_server.port)

    # First, write a EVALSHA that will ask for migration (75% it's on the wrong shard)
    # and some more pipelined commands that will keep Dragonfly busy
    incrs = "".join("INCR a\r\n" for _ in range(50))
    writer.write((f"EVALSHA {sha} 1 a\r\n" + incrs).encode())
    await writer.drain()

    # We migrate only when the socket wakes up, so send another batch to trigger migration
    writer.write("INCR a\r\n".encode())
    await writer.drain()

    # The data doesn't necessarily arrive in a single batch
    async def read():
        reply = ""
        while not reply.strip().endswith("51"):
            reply = (await reader.read(520)).decode()

    # Make sure we recived all replies
    await asyncio.wait_for(read(), timeout=2.0)

    writer.close()
    await writer.wait_closed()


@dfly_args({"proactor_threads": 1})
async def test_large_cmd(async_client: aioredis.Redis):
    MAX_ARR_SIZE = 65535
    res = await async_client.hset(
        "foo", mapping={f"key{i}": f"val{i}" for i in range(MAX_ARR_SIZE // 2)}
    )
    assert res == MAX_ARR_SIZE // 2

    res = await async_client.mset({f"key{i}": f"val{i}" for i in range(MAX_ARR_SIZE // 2)})
    assert res

    res = await async_client.mget([f"key{i}" for i in range(MAX_ARR_SIZE)])
    assert len(res) == MAX_ARR_SIZE


async def test_reject_non_tls_connections_on_tls(with_tls_server_args, df_local_factory):
    server: DflyInstance = df_local_factory.create(
        no_tls_on_admin_port="true",
        admin_port=1111,
        port=1211,
        requirepass="XXX",
        **with_tls_server_args,
    )
    server.start()

    client = server.client(password="XXX")
    with pytest.raises((ResponseError)):
        await client.dbsize()
    await client.close()

    client = server.admin_client(password="XXX")
    assert await client.dbsize() == 0
    await client.close()


async def test_tls_insecure(with_ca_tls_server_args, with_tls_client_args, df_local_factory):
    server = df_local_factory.create(port=BASE_PORT, **with_ca_tls_server_args)
    server.start()

    client = aioredis.Redis(port=server.port, **with_tls_client_args, ssl_cert_reqs=None)
    assert await client.dbsize() == 0
    await client.close()


async def test_tls_full_auth(with_ca_tls_server_args, with_ca_tls_client_args, df_local_factory):
    server = df_local_factory.create(port=BASE_PORT, **with_ca_tls_server_args)
    server.start()

    client = aioredis.Redis(port=server.port, **with_ca_tls_client_args)
    assert await client.dbsize() == 0
    await client.close()


async def test_tls_reject(
    with_ca_tls_server_args, with_tls_client_args, df_local_factory: DflyInstanceFactory
):
    server: DflyInstance = df_local_factory.create(port=BASE_PORT, **with_ca_tls_server_args)
    server.start()

    client = server.client(**with_tls_client_args, ssl_cert_reqs=None)
    await client.ping()
    await client.close()

    client = server.client(**with_tls_client_args)
    with pytest.raises(redis_conn_error):
        await client.ping()
    await client.close()


@dfly_args({"proactor_threads": "4", "pipeline_squash": 10})
async def test_squashed_pipeline(async_client: aioredis.Redis):
    p = async_client.pipeline(transaction=False)

    for j in range(50):
        for i in range(10):
            p.incr(f"k{i}")
        p.execute_command("NOTFOUND")

    res = await p.execute(raise_on_error=False)

    for j in range(50):
        assert res[0:10] == [j + 1] * 10
        assert isinstance(res[10], aioredis.ResponseError)
        res = res[11:]


@dfly_args({"proactor_threads": "4", "pipeline_squash": 10})
async def test_squashed_pipeline_seeder(df_server, df_seeder_factory):
    seeder = df_seeder_factory.create(port=df_server.port, keys=10_000)
    await seeder.run(target_deviation=0.1)


"""
This test makes sure that multi transactions can be integrated into pipeline squashing
"""


@dfly_args({"proactor_threads": "4", "pipeline_squash": 1})
async def test_squashed_pipeline_multi(async_client: aioredis.Redis):
    p = async_client.pipeline(transaction=False)
    for _ in range(5):
        # Series of squashable commands
        for _ in range(5):
            p.set("first", "true")
        # Non-squashable
        p.info()
        # Eval without at tx
        p.execute_command("MULTI")
        p.set("second", "true")
        p.execute_command("EXEC")
        # Finishing sequence
        for _ in range(5):
            p.set("third", "true")
    await p.execute()


async def test_unix_domain_socket(df_local_factory, tmp_dir):
    server = df_local_factory.create(proactor_threads=1, port=BASE_PORT, unixsocket="./df.sock")
    server.start()

    await asyncio.sleep(0.5)

    r = aioredis.Redis(unix_socket_path=tmp_dir / "df.sock")
    assert await r.ping()


async def test_unix_socket_only(df_local_factory, tmp_dir):
    server = df_local_factory.create(proactor_threads=1, port=0, unixsocket="./df.sock")
    server._start()

    await asyncio.sleep(1)

    r = aioredis.Redis(unix_socket_path=tmp_dir / "df.sock")
    assert await r.ping()


"""
Test nested pauses. Executing CLIENT PAUSE should be possible even if another write-pause is active.
It should prolong the pause for all current commands.
"""


@pytest.mark.slow
async def test_nested_client_pause(async_client: aioredis.Redis):
    async def do_pause():
        await async_client.execute_command("CLIENT", "PAUSE", "1000", "WRITE")

    async def do_write():
        await async_client.execute_command("LPUSH", "l", "1")

    p1 = asyncio.create_task(do_pause())
    await asyncio.sleep(0.1)

    p2 = asyncio.create_task(do_write())
    assert not p2.done()

    await asyncio.sleep(0.5)
    p3 = asyncio.create_task(do_pause())

    await p1
    await asyncio.sleep(0.1)
    assert not p2.done()  # blocked by p3 now

    await p2
    await asyncio.sleep(0.0)
    assert p3.done()
    await p3


@dfly_args({"proactor_threads": "4"})
async def test_blocking_command_client_pause(async_client: aioredis.Redis):
    """
    1. Check client pause success when blocking transaction is running
    2. lpush is paused after running client puase
    3. once puased is finished lpush will run and blpop will pop the pushed value
    """

    async def blpop_command():
        res = await async_client.execute_command("blpop dest7 10")
        assert res == ["dest7", "value"]

    async def brpoplpush_command():
        res = await async_client.execute_command("brpoplpush src dest7 2")
        assert res == "value"

    async def lpush_command():
        await async_client.execute_command("lpush src value")

    blpop = asyncio.create_task(blpop_command())
    brpoplpush = asyncio.create_task(brpoplpush_command())
    await asyncio.sleep(0.1)

    res = await async_client.execute_command("client pause 1000")
    assert res == "OK"

    lpush = asyncio.create_task(lpush_command())
    assert not lpush.done()

    await lpush
    await brpoplpush
    await blpop


async def test_multiple_blocking_commands_client_pause(async_client: aioredis.Redis):
    """
    Check running client pause command simultaneously with running multiple blocking command
    from multiple connections
    """

    async def just_blpop():
        key = "".join(random.choices(string.ascii_letters, k=3))
        await async_client.execute_command(f"blpop {key} 2")

    async def client_pause():
        res = await async_client.execute_command("client pause 1000")
        assert res == "OK"

    tasks = [just_blpop() for _ in range(20)]
    tasks.append(client_pause())

    all = asyncio.gather(*tasks)

    assert not all.done()
    await all


@pytest.mark.skip("The test deadlock")
async def test_tls_when_read_write_is_interleaved(
    with_ca_tls_server_args, with_ca_tls_client_args, df_local_factory
):
    """
    This test covers a deadlock bug in helio and TlsSocket when a client connection renegotiated a
    handshake without reading its pending data from the socket.
    This is a weak test case and from our local experiments it deadlocked 30% of the test runs
    """
    server: DflyInstance = df_local_factory.create(
        port=1211, **with_ca_tls_server_args, proactor_threads=1
    )
    # TODO(kostas): to fix the deadlock in the test
    server.start()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    ssl_key = with_ca_tls_client_args["ssl_keyfile"]
    ssl_cert = with_ca_tls_client_args["ssl_certfile"]
    ssl_ca_cert = with_ca_tls_client_args["ssl_ca_certs"]
    ssl_sock = ssl.wrap_socket(
        s,
        keyfile=ssl_key,
        certfile=ssl_cert,
        ca_certs=ssl_ca_cert,
        ssl_version=ssl.PROTOCOL_TLSv1_2,
    )
    ssl_sock.connect(("127.0.0.1", server.port))

    tmp = "f" * 1000
    message = f"SET foo {tmp}\r\n".encode()
    ssl_sock.send(message)

    for i in range(0, 100000):
        res = random.randint(1, 4)
        message = b""
        for j in range(0, res):
            message = message + b"GET foo\r\n"
        ssl_sock.send(message)
        ssl_sock.do_handshake()

    # This deadlocks
    client = aioredis.Redis(port=server.port, **with_ca_tls_client_args)
    await client.execute_command("GET foo")
    await client.close()
