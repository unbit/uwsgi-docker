#! /usr/bin/env python
# coding = utf-8

import unittest
import socket

try:
    import docker
except ImportError:
    from sys import exit
    print("You need docker-py module in order to run these tests.")
    exit()

from truth import CONTAINERS_TRUTH


class TestDockerPlugin(unittest.TestCase):

    def assertDictionariesSubset(self, a, b, msg=None):
        if not msg:
            msg = "the dictionaries are not contained one in the other"

        if type(a) != dict or type(b) != dict:
            raise self.failureException(msg)

        m, n = (a, b) if len(a) <= len(b) else (b, a)

        for k in m:
            try:
                # Nested dictionaries
                if type(m[k]) == dict:
                    try:
                        self.assertDictionariesSubset(m[k], n[k])
                    except self.failureException:
                        print("Error on %s" % k)
                        raise
                # Every other field
                else:
                    if not m[k] == n[k]:
                        print("Error on %s" % k)
                        raise self.failureException(msg)
            except KeyError:
                raise self.failureException(msg)

        return True

    def setUp(self):
        self.client = docker.Client(base_url='unix://var/run/docker.sock',
                                    version='1.12',
                                    timeout=10)
        self.addTypeEqualityFunc(dict, 'assertDictionariesSubset')

    def test_running_containers(self):
        containers = self.client.containers(quiet=False, all=False, trunc=True, latest=False, since=None,
                                            before=None, limit=-1)

        self.assertGreaterEqual(len(containers), len(CONTAINERS_TRUTH))

        count = 0
        for c in containers:
            c_name = c[u'Names'][0]
            inspected_container = self.client.inspect_container(c)

            try:
                self.assertEqual(
                    CONTAINERS_TRUTH[c_name], inspected_container)
                count += 1
            except KeyError:
                # It's ok, there could be other containers running
                pass

            if c_name == u'/vassal_with_standard_socket.ini':
                # Check the pty router
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                # Connect to the socket (mind the permission on the FS!)
                sock.connect('/pty/foo/socket')
                # Rcv from the socket
                s, a = sock.recvfrom(100)  # 100 should be enough
                # Assert the address and that the shell prompt is not ''
                self.assertEqual(a, '/pty/socket')
                self.assertTrue(bool(s))

        self.assertEqual(count, len(CONTAINERS_TRUTH))


unittest.main()
