import pytest
import pymemcache
from . import dfly_args
from .instance import DflyInstance
import socket


@dfly_args({"memcached_port": 11211})
def test_add_get(memcached_connection):
    assert memcached_connection.add(b"key", b"data", noreply=False)
    assert memcached_connection.get(b"key") == b"data"


@dfly_args({"memcached_port": 11211})
def test_add_set(memcached_connection):
    assert memcached_connection.add(b"key", b"data", noreply=False)
    memcached_connection.set(b"key", b"other")
    assert memcached_connection.get(b"key") == b"other"


@dfly_args({"memcached_port": 11211})
def test_set_add(memcached_connection):
    memcached_connection.set(b"key", b"data")
    # stuck here
    assert not memcached_connection.add(b"key", b"other", noreply=False)
    # expects to see NOT_STORED
    memcached_connection.set(b"key", b"other")
    assert memcached_connection.get(b"key") == b"other"


@dfly_args({"memcached_port": 11211})
def test_mixed_reply(memcached_connection):
    memcached_connection.set(b"key", b"data", noreply=True)
    memcached_connection.add(b"key", b"other", noreply=False)
    memcached_connection.add(b"key", b"final", noreply=True)

    assert memcached_connection.get(b"key") == b"data"


@dfly_args({"memcached_port": 11211})
def test_version(memcached_connection: pymemcache.Client):
    """
    php-memcached client expects version to be in the format of "n.n.n",
    so we return 1.5.0 emulating an old memcached server. Our real version is being returned in the stats command. Also verified manually that php client parses correctly the version string
    that ends with "DF".
    """
    assert b"1.5.0 DF" == memcached_connection.version()
    stats = memcached_connection.stats()
    version = stats[b"version"].decode("utf-8")
    assert version.startswith("v") or version == "dev"


@dfly_args({"memcached_port": 11211})
def test_length_in_set_command(df_server: DflyInstance):
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client.connect(("127.0.0.1", 11211))

    command = b"set foo 0 0 4\r\nother\r\n"
    client.sendall(command)
    response = client.recv(256)
    assert response == b"CLIENT_ERROR bad data chunk\r\n"

    command = b"set foo 0 0 4\r\not\r\n\r\n"
    client.sendall(command)
    response = client.recv(256)
    assert response == b"STORED\r\n"

    command = b"set foo 0 0 4\r\n\r\n\r\n\r\n"
    client.sendall(command)
    response = client.recv(256)
    assert response == b"STORED\r\n"

    client.close()
