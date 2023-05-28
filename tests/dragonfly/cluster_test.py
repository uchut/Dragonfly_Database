import pytest
import redis
from redis import asyncio as aioredis
import asyncio

from . import dfly_args

BASE_PORT = 30001


@dfly_args({})
class TestNotEmulated:
    async def test_cluster_commands_fails_when_not_emulate(self, async_client: aioredis.Redis):
        with pytest.raises(aioredis.ResponseError) as respErr:
            await async_client.execute_command("CLUSTER HELP")
        assert "cluster_mode" in str(respErr.value)

        with pytest.raises(aioredis.ResponseError) as respErr:
            await async_client.execute_command("CLUSTER SLOTS")
        assert "emulated" in str(respErr.value)


@dfly_args({"cluster_mode": "emulated"})
class TestEmulated:
    def test_cluster_slots_command(self, cluster_client: redis.RedisCluster):
        expected = {(0, 16383): {'primary': (
            '127.0.0.1', 6379), 'replicas': []}}
        res = cluster_client.execute_command("CLUSTER SLOTS")
        assert expected == res

    def test_cluster_help_command(self, cluster_client: redis.RedisCluster):
        # `target_nodes` is necessary because CLUSTER HELP is not mapped on redis-py
        res = cluster_client.execute_command(
            "CLUSTER HELP", target_nodes=redis.RedisCluster.RANDOM)
        assert "HELP" in res
        assert "SLOTS" in res

    def test_cluster_pipeline(self, cluster_client: redis.RedisCluster):
        pipeline = cluster_client.pipeline()
        pipeline.set("foo", "bar")
        pipeline.get("foo")
        val = pipeline.execute()
        assert val == [True, "bar"]


@dfly_args({"cluster_mode": "emulated", "cluster_announce_ip": "127.0.0.2"})
class TestEmulatedWithAnnounceIp:
    def test_cluster_slots_command(self, cluster_client: redis.RedisCluster):
        expected = {(0, 16383): {'primary': (
            '127.0.0.2', 6379), 'replicas': []}}
        res = cluster_client.execute_command("CLUSTER SLOTS")
        assert expected == res


def verify_slots_result(ip: str, port: int, answer: list, rep_ip: str = None, rep_port: int = None) -> bool:
    def is_local_host(ip: str) -> bool:
        return ip == '127.0.0.1' or ip == 'localhost'

    assert answer[0] == 0       # start shard
    assert answer[1] == 16383   # last shard
    if rep_ip is not None:
        assert len(answer) == 4  # the network info
        rep_info = answer[3]
        assert len(rep_info) == 3
        ip_addr = str(rep_info[0], 'utf-8')
        assert ip_addr == rep_ip or (
            is_local_host(ip_addr) and is_local_host(ip))
        assert rep_info[1] == rep_port
    else:
        assert len(answer) == 3
    info = answer[2]
    assert len(info) == 3
    ip_addr = str(info[0], 'utf-8')
    assert ip_addr == ip or (is_local_host(ip_addr) and is_local_host(ip))
    assert info[1] == port
    return True


@dfly_args({"proactor_threads": 4, "cluster_mode": "emulated"})
async def test_cluster_slots_in_replicas(df_local_factory):
    master = df_local_factory.create(port=BASE_PORT)
    replica = df_local_factory.create(port=BASE_PORT+1, logtostdout=True)

    df_local_factory.start_all([master, replica])

    c_master = aioredis.Redis(port=master.port)
    c_replica = aioredis.Redis(port=replica.port)

    res = await c_replica.execute_command("CLUSTER SLOTS")
    assert len(res) == 1
    assert verify_slots_result(
        ip="127.0.0.1", port=replica.port, answer=res[0])
    res = await c_master.execute_command("CLUSTER SLOTS")
    assert verify_slots_result(
        ip="127.0.0.1", port=master.port, answer=res[0])

    # Connect replica to master
    rc = await c_replica.execute_command(f"REPLICAOF localhost {master.port}")
    assert str(rc, 'utf-8') == "OK"
    await asyncio.sleep(0.5)
    res = await c_replica.execute_command("CLUSTER SLOTS")
    assert verify_slots_result(
        ip="127.0.0.1", port=master.port, answer=res[0], rep_ip="127.0.0.1", rep_port=replica.port)
    res = await c_master.execute_command("CLUSTER SLOTS")
    assert verify_slots_result(
        ip="127.0.0.1", port=master.port, answer=res[0], rep_ip="127.0.0.1", rep_port=replica.port)


@dfly_args({"cluster_mode": "emulated", "cluster_announce_ip": "127.0.0.2"})
async def test_cluster_info(async_client):
    res = await async_client.execute_command("CLUSTER INFO")
    assert len(res) == 16
    assert res == {'cluster_current_epoch': '1',
                   'cluster_known_nodes': '1',
                   'cluster_my_epoch': '1',
                   'cluster_size': '1',
                   'cluster_slots_assigned': '16384',
                   'cluster_slots_fail': '0',
                   'cluster_slots_ok': '16384',
                   'cluster_slots_pfail': '0',
                   'cluster_state': 'ok',
                   'cluster_stats_messages_meet_received': '0',
                   'cluster_stats_messages_ping_received': '1',
                   'cluster_stats_messages_ping_sent': '1',
                   'cluster_stats_messages_pong_received': '1',
                   'cluster_stats_messages_pong_sent': '1',
                   'cluster_stats_messages_received': '1',
                   'cluster_stats_messages_sent': '1'
                   }


@dfly_args({"cluster_mode": "emulated", "cluster_announce_ip": "127.0.0.2"})
@pytest.mark.asyncio
async def test_cluster_nodes(async_client):
    res = await async_client.execute_command("CLUSTER NODES")
    assert len(res) == 1
    info = res['127.0.0.2:6379']
    assert res is not None
    assert info['connected'] == True
    assert info['epoch'] == '0'
    assert info['flags'] == 'myself,master'
    assert info['last_ping_sent'] == '0'
    assert info['slots'] == [['0', '16383']]
    assert info['master_id'] == "-"
