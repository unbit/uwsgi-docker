uwsgi-docker
============

uWSGI plugin for integration with Docker

This plugin allows you to configure and run vassals in docker containers.

Once the docker daemon is started and you have a bunch of images, you only need to edit
vassal's configuration to dockerize it.

Requirements
============

You need at least uWSGI 2.1 for the Emperor and uWSGI 2.0.6 for vassals.

For a better experience you should use uWSGI >= 2.1 even in vassals to get advanced features, like binding a socket in the host and passing it to the docker instance without worrying about redirecting ports or mapping additional filesystems.

To build the plugin you need libcurl (and its development headers) and libjansson (and its development headers)

A little understanding of docker and how it works is highly suggested

Obviously only Linux is supported.

Quickstart
==========

Build the plugin (the plugin is only required for the Emperor, vassals are not aware of their dockerization):

```sh
uwsgi --build-plugin https://github.com/unbit/uwsgi-docker
```

Ensure the docker daemon is running (eventually run it from the terminal as root with 'docker -d')

Prepare your first image (this will be for a perl/psgi app based on ubuntu):

```sh
docker run -t -i --name=psgi_image ubuntu /bin/bash
```

you are now in a new container (named 'psgi_image') and you can start installing the required packages, a bunch of cpan modules, uwsgi and a simple hello world psgi app

```sh
apt-get update
apt-get install python libperl-dev build-essential libpcre3-dev cpanminus
apt-get clean
cpanm Plack
curl http://uwsgi.it/install | bash -s psgi /usr/bin/uwsgi
mkdir /var/www
echo "my \$app = sub { return [200, [], ['Hello World from Docker']];}" > /var/www/app.pl
```

now, let's commit the image as 'psgi001' (run the following command in another terminal)

```sh
docker commit psgi_image psgi001
```

and now we are free to destroy the container

```sh
docker stop psgi_image
docker rm psgi_image
```

The psgi001 image is ready to be used, let's prepare a vassal for our psgi app

```ini
; the [emperor] section is parsed ONLY by the Emperor
[emperor]
; use psgi001 as the docker image
docker-image = psgi001
; map host port 9001 to container port 3031/tcp
docker-port = 9001:3031
; map host port 127.0.0.1:5001 to container port 5000/tcp
docker-port = 127.0.0.1:5001:5000

[uwsgi]
psgi = /var/www/app.pl
processes = 4
; in bridged mode the class B network is allocated, so we can simply bind to the first address starting with 172
; so we use the very handy .* trick
socket = 172.*:3031
uid = www-data
gid = www-data
; we are free to bind the stats server to all of the address
; docker will protect it
stats = :5000
```

And now we can finally run the Emperor:

```ini
[uwsgi]
plugin = path_to/docker_plugin.so
emperor = dir:///etc/uwsgi
; once the plugin is loaded, docker support is optional
; this option disallow running vassals without docker
emperor-docker-required = true
; use /usr/bin/uwsgi as the container entry point (the path coudl be different from the Emperor one, so we force it)
emperor-wrapper = /usr/bin/uwsgi

```

Simplyfing socket management (requires uWSGI 2.1 in vassals)
============================================================

One of the most common needs in app management is allowing connections from the frontend web proxy to the backend applications. As when under docker, applications run in a different network namespace, you need some way to enable communication between them.

In the quickstart example we have seen how to forward ports from the host to the container. You can obtain the same with UNIX sockets mounting a host directory (where sockets are bound) to the container.

Both are easy to accomplish, but are suboptimal:

- when mounting a dir you will be very probably forced to show all of the bound sockets to the container (can be a privacy problem, expecially if you give sockets a meaningful name)

- when forwarding ports, you need to choose which port mapping, this could be annoying and error prone.

- forwarding ports has its overhead (albeit all managed in kernel space via iptables)

The docker plugin exposes a 'docker-socket' attribute, allowing you to bind a socket in the host and automatically pass it to the docker instance. Our previous example can be simplified in this way:

```ini
; the [emperor] section is parsed ONLY by the Emperor
[emperor]
; use psgi001 as the docker image
docker-image = psgi001
; bind to address /var/run/example.com.socket
docker-socket = /var/run/example.com.socket

[uwsgi]
psgi = /var/www/app.pl
processes = 4
uid = www-data
gid = www-data
```
The host bind to /var/run/example.com.socket and passes it to the dockerized vassal automatically.

Now you only need to configure your proxy (nginx, apache ...) to connect to /var/run/example.com.socket

You can bind to both UNIX and INET sockets.

Remember, you need uWSGI 2.1 in the vassal for supporting socket passing (albeit very probably uWSGI 2.0.8 will include this feature)

How it works
============

The plugin communicates with the docker daemon unix socket (/var/run/docker.sock).

On vassal start the plugin ask the docker daemon to create a new container with specific attributes and with same name of the vassal (a foobar.ini vassal will generate a foobar.ini docker container). This container automatically get access to a unix socket (the proxy emperor socket) from which it will gets the emperor file descriptors (control pipe, configuration pipe and eventually the on-demand socket). Once the emperor proxy has passed the file descriptor it is completely destroyed (as from now on the Emperor has full control over the dockerized instance).

For security and robustness the docker transaction is managed by a process for each vassal. This process is named [uwsgi-docker-bridge] and it requires very few memory. Once this process has completed the spawn of the instance it attaches itself to the pseudoterminal of the docker instance (so you will transparently get instance logs if not redirected). Once the pseudoterminal closes, the vassal is destroyed too.

If during the startup phase of the vassal, a docker instance named as the vassal is found, it will be automatically destroyed.

When the emperor dies, all of the related containers are destroyed too.

The Emperor Proxy
=================

As seen before the dockerized instance requires access to a special unix socket (the emperor proxy socket) that will pass al of the required file descriptors.

By the default this socket is created in the same directory of the vassal file suffixing it with .sock.

/etc/uwsgi/foobar.ini will generate /etc/uwsgi/foobar.ini.sock that will be mounted as /foobar.ini.sock

You can override this behaviour using the the `docker-proxy` attribute:

```ini
; the [emperor] section is parsed ONLY by the Emperor
[emperor]
; use psgi001 as the docker image
docker-image = psgi001
; bind to address /var/run/example.com.socket
docker-socket = /var/run/example.com.socket

docker-proxy=/var/run/foobar.sock:/tmp/foosocket

[uwsgi]
psgi = /var/www/app.pl
processes = 4
uid = www-data
gid = www-data
```
this will create a /var/run/foobar.sock unix socket in the host, exposed to the docker instance as /tmp/foosocket.

Remember that the unix socket file is removed from the host soon after the docker instance has connected to it.

An important thing YOU HAVE TO REMEMBER, is that the uwsgi process run by the instance must have write access to it, so if you are using the `docker-user` attribute (to start uwsgi as an unprivileged user instead of configuring it for dropping privileges) you have to tell the emperor to create sockets with a specific permission mode using the classic `chmod-socket` option

```ini
[uwsgi]
plugin = path_to/docker_plugin.so
emperor = dir:///etc/uwsgi
; once the plugin is loaded, docker support is optional
; this option disallow running vassals without docker
emperor-docker-required = true
; use /usr/bin/uwsgi as the container entry point (the path coudl be different from the Emperor one, so we force it)
emperor-wrapper = /usr/bin/uwsgi

chmod-socket = 666
```

Remember the socket is suddenly destroyed after the first connection.


Integration with the forkpty router plugin
==========================================

The forkpty router is a plugin included in the official uWSGI sources. It allows you to attach a terminal session
to a running uWSGI instance:

http://uwsgi-docs.readthedocs.org/en/latest/ForkptyRouter.html

Best approach is exposing an host directory for the vassal in the docker container. You will be able to use this directory to create the unix socket of the forkpty router (another approach would be binding it on a inet socket and forwarding the port).

```ini
; the [emperor] section is parsed ONLY by the Emperor
[emperor]
; use psgi001 as the docker image
docker-image = psgi001
; bind to address /var/run/example.com.socket
docker-socket = /var/run/example.com.socket

; mount /pty/foo as /foo (ensure it is owned by www-data)
docker-mount = /pty/foo:/pty

[uwsgi]
psgi = /var/www/app.pl
processes = 4
uid = www-data
gid = www-data
forkpty-router = /pty/socket
```

Now you can enter the container with:

```sh
uwsgi --pty-connect /pty/foo/socket
```

Remember to rebuild uWSGI with `pty` and `forkptyrouter` plugins embedded, a fast trick is using

```sh
UWSGI_EMBED_PLUGINS=pty,forkptyrouter make psgi
```

Technically the host instance requires only the `pty` one, while the dockerized one the `forkptyrouter` one.

Eventually you can build them as external plugins and load dinamically:

```sh
uwsgi --build-plugin plugins/pty
uwsgi --build-plugin plugins/forkptyrouter
```

By default the forkptyrouter runs /bin/sh. /bin/bash should be a better choice, just set it with `forkptyrouter-command` option.

```ini
; the [emperor] section is parsed ONLY by the Emperor
[emperor]
; use psgi001 as the docker image
docker-image = psgi001
; bind to address /var/run/example.com.socket
docker-socket = /var/run/example.com.socket

; mount /pty/foo as /foo (ensure it is owned by www-data)
docker-mount = /pty/foo:/pty

[uwsgi]
plugin = path_to/forkptyrouter_plugin.so
psgi = /var/www/app.pl
processes = 4
uid = www-data
gid = www-data
forkpty-router = /pty/socket
forkptyrouter-command = /bin/bash
```

The forkptyrouter can be used instead of `nsenter` as it does not require special privileges (your client needs only write access to the specific unix socket)

Attributes
==========

`docker-image` set the image to use for the container

`docker-socket` bind a socket and pass it to the docker instance

`docker-port` forward a port, syntax hostip:hostport:dockerport or hostport:dockerport

`docker-workdir` set working directory

`docker-mount` bind mount from host to docker, syntax /host:/docker

`docker-network-mode` set network mode (bridge, host, none, container:id)

`docker-hostname` set docker instance hostname

`docker-proxy` set emperor proxy path, syntax hostpath:dockerpath

`docker-env` set environment variable in docker instance

`docker-user` run uwsgi in docker as the specified user (otherwise it will start as root and you will need to specify uid and gid in the vassal)

`docker-memory` set the max amount of memory (in bytes) for the docker instance

`docker-swap` set the max amount of swap memory (in bytes) for the docker instance

`docker-cidfile` store the cid (container id) file to the specified path

`docker-dns` add a dns server to the docker instance

Options
=======

`--docker-emperor/--emperor-docker` enable the docker support in the Emperor

`--docker-emperor-required/--emperor-docker-required` enable the docker support in the Emperor and requires each vassal to expose Docker options

`--docker-debug` enable debug logging

`--docker-daemon-socket` change the default docker daemon socker (default /var/run/docker.sock)

