import pytest
import redis
from redis import asyncio as aioredis
from . import DflyInstanceFactory
from .utility import disconnect_clients
import asyncio


@pytest.mark.asyncio
async def test_acl_list_default_user(async_client):
    """
    make sure that the default created user is printed correctly
    """

    # Bad input
    with pytest.raises(redis.exceptions.ResponseError):
        await async_client.execute_command("ACL LIST TEMP")

    with pytest.raises(redis.exceptions.ResponseError):
        await async_client.execute_command("ACL")

    result = await async_client.execute_command("ACL LIST")
    assert 1 == len(result)
    assert "user default on nopass +@ALL" == result[0]


@pytest.mark.asyncio
async def test_acl_setuser(async_client):
    # Bad input
    with pytest.raises(redis.exceptions.ResponseError):
        await async_client.execute_command("ACL SETUSER")

    await async_client.execute_command("ACL SETUSER kostas")
    result = await async_client.execute_command("ACL LIST")
    assert 2 == len(result)
    assert "user kostas off nopass +@NONE" in result

    await async_client.execute_command("ACL SETUSER kostas ON")
    result = await async_client.execute_command("ACL LIST")
    assert "user kostas on nopass +@NONE" in result

    await async_client.execute_command("ACL SETUSER kostas +@list +@string +@admin")
    result = await async_client.execute_command("ACL LIST")
    # TODO consider printing to lowercase
    assert "user kostas on nopass +@LIST +@STRING +@ADMIN" in result

    await async_client.execute_command("ACL SETUSER kostas -@list -@admin")
    result = await async_client.execute_command("ACL LIST")
    assert "user kostas on nopass +@STRING" in result

    # mix and match
    await async_client.execute_command("ACL SETUSER kostas +@list -@string")
    result = await async_client.execute_command("ACL LIST")
    assert "user kostas on nopass +@LIST" in result

    # mix and match interleaved
    await async_client.execute_command("ACL SETUSER kostas +@set -@set +@set")
    result = await async_client.execute_command("ACL LIST")
    assert "user kostas on nopass +@SET +@LIST" in result

    await async_client.execute_command("ACL SETUSER kostas +@all")
    result = await async_client.execute_command("ACL LIST")
    assert "user kostas on nopass +@ALL" in result


@pytest.mark.asyncio
async def test_acl_auth(async_client):
    await async_client.execute_command("ACL SETUSER shahar >mypass")

    with pytest.raises(redis.exceptions.ResponseError):
        await async_client.execute_command("AUTH shahar wrong_pass")

    # This should fail because user is inactive
    with pytest.raises(redis.exceptions.ResponseError):
        await async_client.execute_command("AUTH shahar mypass")

    # Activate user
    await async_client.execute_command("ACL SETUSER shahar ON +@fast")

    result = await async_client.execute_command("AUTH shahar mypass")
    assert result == "OK"

    # Let's also try default
    result = await async_client.execute_command("AUTH default nopass")
    assert result == "OK"


@pytest.mark.asyncio
async def test_acl_categories(async_client):
    await async_client.execute_command("ACL SETUSER vlad ON >mypass +@string +@list +@connection")

    result = await async_client.execute_command("AUTH vlad mypass")
    assert result == "OK"

    result = await async_client.execute_command("SET foo bar")
    assert result == "OK"

    result = await async_client.execute_command("LPUSH mykey space_monkey")
    assert result == 1

    # This should fail, vlad does not have @admin
    with pytest.raises(redis.exceptions.ResponseError):
        await async_client.execute_command("ACL SETUSER vlad ON >mypass")

    # This should fail, vlad does not have @sortedset
    with pytest.raises(redis.exceptions.ResponseError):
        await async_client.execute_command("ZADD myset 1 two")

    result = await async_client.execute_command("AUTH default nopass")
    assert result == "OK"

    # Make vlad an admin
    await async_client.execute_command("ACL SETUSER vlad -@string")
    assert result == "OK"

    result = await async_client.execute_command("AUTH vlad mypass")
    assert result == "OK"

    with pytest.raises(redis.exceptions.ResponseError):
        await async_client.execute_command("GET foo")

    result = await async_client.execute_command("AUTH default nopass")
    assert result == "OK"

    # Vlad goes rogue starts giving admin stats to random users
    await async_client.execute_command("ACL SETUSER adi >adi +@admin")
    assert result == "OK"

    # Vlad can now execute everything
    await async_client.execute_command("ACL SETUSER vlad +@all")
    assert result == "OK"

    await async_client.execute_command("ZADD myset 1 two")
    assert result == "OK"


@pytest.mark.asyncio
async def test_acl_categories_multi_exec_squash(df_local_factory):
    df = df_local_factory.create(multi_exec_squash=True, port=1111)

    df.start()

    client = aioredis.Redis(port=df.port)
    res = await client.execute_command("ACL SETUSER kk ON >kk +@transaction +@string")
    assert res == b"OK"

    res = await client.execute_command("AUTH kk kk")
    assert res == b"OK"

    await client.execute_command("MULTI")
    assert res == b"OK"
    for x in range(33):
        await client.execute_command(f"SET x{x} {x}")
    await client.execute_command("EXEC")

    client = aioredis.Redis(port=df.port)
    await client.close()

    # NOPERM while executing multi
    await client.execute_command("ACL SETUSER kk -@string")
    assert res == b"OK"
    await client.execute_command("AUTH kk kk")
    assert res == b"OK"
    await client.execute_command("MULTI")
    assert res == b"OK"

    with pytest.raises(redis.exceptions.ResponseError):
        await client.execute_command(f"SET x{x} {x}")
    await client.close()

    # NOPERM between multi and exec
    admin_client = aioredis.Redis(port=df.port)
    res = await client.execute_command("ACL SETUSER kk +@string")
    assert res == b"OK"

    client = aioredis.Redis(port=df.port)
    res = await client.execute_command("AUTH kk kk")
    assert res == b"OK"
    # CLIENT has permissions, starts MULTI and issues a bunch of SET commands
    await client.execute_command("MULTI")
    assert res == b"OK"
    for x in range(33):
        await client.execute_command(f"SET x{x} {x}")

    # ADMIN revokes permissions
    res = await admin_client.execute_command("ACL SETUSER kk -@string")
    assert res == b"OK"

    res = await client.execute_command("EXEC")
    # TODO(we need to fix this, basiscally SQUASHED/MULTI transaction commands
    # return multiple errors for each command failed. Since the nature of the error
    # is the same, that a rule has changed we should squash those error messages into
    # one.
    assert res[0].args[0] == "NOPERM: kk ACL rules changed between the MULTI and EXEC"

    await admin_client.close()
    await client.close()


@pytest.mark.asyncio
async def test_acl_deluser(df_server):
    client = aioredis.Redis(port=df_server.port)

    with pytest.raises(redis.exceptions.ResponseError):
        await client.execute_command("ACL DELUSER random")

    res = await client.execute_command("ACL SETUSER george ON >pass +@transaction +@string")
    assert res == b"OK"

    res = await client.execute_command("AUTH george pass")
    assert res == b"OK"

    await client.execute_command("MULTI")
    await client.execute_command("SET key 44")

    admin_client = aioredis.Redis(port=df_server.port)
    await admin_client.execute_command("ACL DELUSER george")

    with pytest.raises(redis.exceptions.ConnectionError):
        await client.execute_command("EXEC")

    await admin_client.close()


script = """
for i = 1, 100000 do
  redis.call('SET', 'key', i)
  redis.call('SET', 'key1', i)
  redis.call('SET', 'key2', i)
  redis.call('SET', 'key3', i)
end
"""


@pytest.mark.asyncio
@pytest.mark.skip("Non deterministic")
async def test_acl_del_user_while_running_lua_script(df_server):
    client = aioredis.Redis(port=df_server.port)
    await client.execute_command("ACL SETUSER kostas ON >kk +@string +@scripting")
    await client.execute_command("AUTH kostas kk")
    admin_client = aioredis.Redis(port=df_server.port)

    with pytest.raises(redis.exceptions.ConnectionError):
        await asyncio.gather(
            client.eval(script, 4, "key", "key1", "key2", "key3"),
            admin_client.execute_command("ACL DELUSER kostas"),
        )

    for i in range(1, 4):
        res = await admin_client.get(f"key{i}")
        assert res == b"100000"

    await admin_client.close()


@pytest.mark.asyncio
@pytest.mark.skip("Non deterministic")
async def test_acl_with_long_running_script(df_server):
    client = aioredis.Redis(port=df_server.port)
    await client.execute_command("ACL SETUSER roman ON >yoman +@string +@scripting")
    await client.execute_command("AUTH roman yoman")
    admin_client = aioredis.Redis(port=df_server.port)

    await asyncio.gather(
        client.eval(script, 4, "key", "key1", "key2", "key3"),
        admin_client.execute_command("ACL SETUSER roman -@string -@scripting"),
    )

    for i in range(1, 4):
        res = await admin_client.get(f"key{i}")
        assert res == b"100000"

    await client.close()
    await admin_client.close()


@pytest.mark.asyncio
async def test_acl_whoami(async_client):
    await async_client.execute_command("ACL SETUSER kostas >kk +@ALL ON")

    with pytest.raises(redis.exceptions.ResponseError):
        await async_client.execute_command("ACL WHOAMI WHO")

    result = await async_client.execute_command("ACL WHOAMI")
    assert result == "User is default"

    result = await async_client.execute_command("AUTH kostas kk")

    result = await async_client.execute_command("ACL WHOAMI")
    assert result == "User is kostas"
