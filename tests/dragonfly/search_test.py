"""
Test compatibility with the redis-py client search module.
Search correctness should be ensured with unit tests.
"""
import pytest
from redis import asyncio as aioredis
from .utility import *

from redis.commands.search.field import TextField, NumericField, TagField
from redis.commands.search.indexDefinition import IndexDefinition, IndexType

TEST_DATA = [
    {"title": "First article", "content": "Long description",
        "views": 100, "topic": "world, science"},

    {"title": "Second article", "content": "Small text",
        "views": 200, "topic": "national, policits"},

    {"title": "Third piece", "content": "Brief description",
        "views": 300, "topic": "health, lifestyle"},

    {"title": "Last piece", "content": "Interesting text",
        "views": 400, "topic": "world, business"},
]

TEST_DATA_SCHEMA = [TextField("title"), TextField(
    "content"), NumericField("views"), TagField("topic")]


async def index_test_data(async_client: aioredis.Redis, itype: IndexType, prefix=""):
    for i, e in enumerate(TEST_DATA):
        if itype == IndexType.HASH:
            await async_client.hset(prefix+str(i), mapping=e)
        else:
            await async_client.json().set(prefix+str(i), "$", e)

def doc_to_str(doc):
    if not type(doc) is dict:
        doc = doc.__dict__

    doc = dict(doc) # copy to remove fields
    doc.pop('id', None)
    doc.pop('payload', None)

    return '//'.join(sorted(doc))

def contains_test_data(res, td_indices):
    if res.total != len(td_indices):
        return False

    docset = {doc_to_str(doc) for doc in res.docs}

    for td_entry in (TEST_DATA[tdi] for tdi in td_indices):
        if not doc_to_str(td_entry) in docset:
            return False

    return True


@pytest.mark.parametrize("index_type", [IndexType.HASH, IndexType.JSON])
async def test_basic(async_client, index_type):
    i1 = async_client.ft("i-"+str(index_type))
    await index_test_data(async_client, index_type)
    await i1.create_index(TEST_DATA_SCHEMA, definition=IndexDefinition(index_type=index_type))

    res = await i1.search("article")
    assert contains_test_data(res, [0, 1])

    res = await i1.search("text")
    assert contains_test_data(res, [1, 3])

    res = await i1.search("brief piece")
    assert contains_test_data(res, [2])

    res = await i1.search("@title:(article|last) @content:text")
    assert contains_test_data(res, [1, 3])

    res = await i1.search("@views:[200 300]")
    assert contains_test_data(res, [1, 2])

    res = await i1.search("@views:[0 150] | @views:[350 500]")
    assert contains_test_data(res, [0, 3])

    res = await i1.search("@topic:{world}")
    assert contains_test_data(res, [0, 3])

    res = await i1.search("@topic:{business}")
    assert contains_test_data(res, [3])

    res = await i1.search("@topic:{world | national}")
    assert contains_test_data(res, [0, 1, 3])

    res = await i1.search("@topic:{science | health}")
    assert contains_test_data(res, [0, 2])
