import aioredis
import asyncio
import time
import json
import pytest
import random
from . import dfly_args, dfly_multi_test_args

DJANGO_CACHEOPS_SCRIPT = """
local prefix = KEYS[1]
local key = KEYS[2]
local precall_key = KEYS[3]
local data = ARGV[1]
local dnfs = cjson.decode(ARGV[2])
local timeout = tonumber(ARGV[3])

if precall_key ~= prefix and redis.call('exists', precall_key) == 0 then
  -- Cached data was invalidated during the function call. The data is
  -- stale and should not be cached.
  return
end

-- Write data to cache
redis.call('setex', key, timeout, data)


-- A pair of funcs
-- NOTE: we depend here on keys order being stable
local conj_schema = function (conj)
    local parts = {}
    for field, _ in pairs(conj) do
        table.insert(parts, field)
    end

    return table.concat(parts, ',')
end

local conj_cache_key = function (db_table, conj)
    local parts = {}
    for field, val in pairs(conj) do
        table.insert(parts, field .. '=' .. tostring(val))
    end

    return prefix .. 'conj:' .. db_table .. ':' .. table.concat(parts, '&')
end


-- Update schemes and invalidators
for db_table, disj in pairs(dnfs) do
    for _, conj in ipairs(disj) do
        -- Ensure scheme is known
        redis.call('sadd', prefix .. 'schemes:' .. db_table, conj_schema(conj))

        -- Add new cache_key to list of dependencies
        local conj_key = conj_cache_key(db_table, conj)

        redis.call('sadd', conj_key, key)
        -- NOTE: an invalidator should live longer than any key it references.
        --       So we update its ttl on every key if needed.
        -- NOTE: if CACHEOPS_LRU is True when invalidators should be left persistent,
        --       so we strip next section from this script.
        -- TOSTRIP
        local conj_ttl = redis.call('ttl', conj_key)
        if conj_ttl < timeout then
            -- We set conj_key life with a margin over key life to call expire rarer
            -- And add few extra seconds to be extra safe
            redis.call('expire', conj_key, timeout * 2 + 10)
        end
        -- /TOSTRIP
    end
end

return 'OK'
"""


def DJANGO_CACHEOPS_SCHEMA(vs): return {
    "table_1": [
        {"f-1": f'v-{vs[0]}'}, {"f-2": f'v-{vs[1]}'}
    ],
    "table_2": [
        {"f-1": f'v-{vs[2]}'}, {"f-2": f'v-{vs[3]}'}
    ]
}


"""
Test the main caching script of https://github.com/Suor/django-cacheops.
The script accesses undeclared keys (that are built based on argument data),
so Dragonfly must run in global (1) or non-atomic (4) multi eval mode.
"""


@dfly_multi_test_args({'default_lua_config': 'allow-undeclared-keys', 'proactor_threads': 4},
                      {'default_lua_config': 'allow-undeclared-keys disable-atomicity', 'proactor_threads': 4})
async def test_django_cacheops_script(async_client, num_keys=500):
    script = async_client.register_script(DJANGO_CACHEOPS_SCRIPT)

    data = [(f'k-{k}', [random.randint(0, 10) for _ in range(4)])
            for k in range(num_keys)]
    for k, vs in data:
        schema = DJANGO_CACHEOPS_SCHEMA(vs)
        assert await script(keys=['', k, ''], args=['a' * 10, json.dumps(schema, sort_keys=True), 100]) == 'OK'

    # Check schema was built correctly
    base_schema = DJANGO_CACHEOPS_SCHEMA([0] * 4)
    for table, fields in base_schema.items():
        schema = await async_client.smembers(f'schemes:{table}')
        fields = set.union(*(set(part.keys()) for part in fields))
        assert schema == fields

    # Check revese mapping is correct
    for k, vs in data:
        assert await async_client.exists(k)
        for table, fields in DJANGO_CACHEOPS_SCHEMA(vs).items():
            for sub_schema in fields:
                conj_key = f'conj:{table}:' + \
                    '&'.join("{}={}".format(f, v)
                             for f, v in sub_schema.items())
                assert await async_client.sismember(conj_key, k)


ASYNQ_ENQUEUE_SCRIPT = """
if redis.call("EXISTS", KEYS[1]) == 1 then
	return 0
end
redis.call("HSET", KEYS[1],
           "msg", ARGV[1],
           "state", "pending",
           "pending_since", ARGV[3])
redis.call("LPUSH", KEYS[2], ARGV[2])
return 1
"""

ASYNQ_DEQUE_SCRIPT = """
if redis.call("EXISTS", KEYS[2]) == 0 then
	local id = redis.call("RPOPLPUSH", KEYS[1], KEYS[3])
	if id then
		local key = ARGV[2] .. id
		redis.call("HSET", key, "state", "active")
		redis.call("HDEL", key, "pending_since")
		redis.call("ZADD", KEYS[4], ARGV[1], id)
		return redis.call("HGET", key, "msg")
	end
end
return nil
"""

"""
Test the main queueing scripts of https://github.com/hibiken/asynq.
The deque script accesses undeclared keys (that are popped from a list),
so Dragonfly must run in global (1) or non-atomic (4) multi eval mode.

Running the deque script in non-atomic mode can introduce inconsistency to an outside observer.
For example, an item can be already placed into the active queue (RPUSH KEYS[3]), buts its state in the hash
wasn't yet updated to active. Because we only access keys that we popped from the list (RPOPLPUSH is still atomic by itself),
the task system should work reliably.
"""


@dfly_multi_test_args({'default_lua_config': 'allow-undeclared-keys', 'proactor_threads': 4},
                      {'default_lua_config': 'allow-undeclared-keys disable-atomicity', 'proactor_threads': 4})
async def test_golang_asynq_script(async_pool, num_queues=10, num_tasks=100):
    async def enqueue_worker(queue):
        client = aioredis.Redis(connection_pool=async_pool)
        enqueue = client.register_script(ASYNQ_ENQUEUE_SCRIPT)

        task_ids = 2*list(range(num_tasks))
        random.shuffle(task_ids)
        res = [await enqueue(keys=[f"asynq:{{{queue}}}:t:{task_id}", f"asynq:{{{queue}}}:pending"],
                             args=[f"{task_id}", task_id, int(time.time())])
               for task_id in task_ids]

        assert sum(res) == num_tasks

    # Start filling the queues
    jobs = [asyncio.create_task(enqueue_worker(
        f"q-{queue}")) for queue in range(num_queues)]


    collected = 0

    async def dequeue_worker():
        nonlocal collected
        client = aioredis.Redis(connection_pool=async_pool)
        dequeue = client.register_script(ASYNQ_DEQUE_SCRIPT)

        while collected < num_tasks * num_queues:
            #pct = round(collected/(num_tasks*num_queues), 2)
            #print(f'\r    \r{pct}', end='', flush=True)
            for queue in (f"q-{queue}" for queue in range(num_queues)):
                prefix = f"asynq:{{{queue}}}:t:"
                msg = await dequeue(keys=[f"asynq:{{{queue}}}:"+t for t in ["pending", "paused", "active", "lease"]],
                                    args=[int(time.time()), prefix])
                if msg is not None:
                    collected += 1
                    assert await client.hget(prefix+msg, 'state') == 'active'

    # Run many contending workers
    await asyncio.gather(*(dequeue_worker() for _ in range(num_queues * 2)))

    for job in jobs:
        await job

ERROR_CALL_SCRIPT = """
redis.call('ECHO', 'I', 'want', 'an', 'error')
"""

ERROR_PCALL_SCRIPT = """
redis.pcall('ECHO', 'I', 'want', 'an', 'error')
"""

@pytest.mark.asyncio
async def test_eval_error_propagation(async_client):
    assert await async_client.eval(ERROR_PCALL_SCRIPT, 0) is None

    try:
        await async_client.eval(ERROR_CALL_SCRIPT, 0)
        assert False, "Eval must have thrown an error"
    except aioredis.RedisError as e:
        pass
