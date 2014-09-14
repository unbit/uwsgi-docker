*** WORK IN PROGRESS ***

uwsgi-docker
============

uWSGI plugin for integration with Docker

This plugin allows you to configure and run vassals in docker containers.

Once the docker daemon is started and you have a bunch of images, you only need to edit
vassal's configuration to dockerize it.

Requirements
============

You need at least uWSGI 2.1 for the Emperor and uWSGI 2.0.6 for vassals

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
echo "my \$app = sub { return [200, {}, ['Hello World from Docker']];}" > /var/www/app.pl
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

How it works
============

The plugin communicates with the docker daemon unix socket (/var/run/docker.sock).

On vassal start the plugin ask the docker daemon to create a new container with specific attributes and with same name of the vassal (a foobar.ini vassal will generate a foobar.ini docker container). This container automatically get access to a unix socket (the proxy emperor socket) from which it will gets the emperor file descriptors (control pipe, configuration pipe and eventually the on-demand socket). Once the emperor proxy has passed the file descriptor it is completely destroyed (as from now on the Emperor has full control over the dockerized instance).

For security and robustness the docker transaction is managed by a process for each vassal. This process is named [uwsgi-docker-bridge] and it requires very few memory. Once this process has completed the spawn of the instance it attaches itself to the pseudoterminal of the docker instance (so you will transparently get instance logs if not redirected). Once the pseudoterminal closes, the vassal is destroyed too.



Attributes
==========

docker-image

docker-port

docker-workdir

docker-mount

docker-disable-network

docker-network-mode

docker-hostname

docker-proxy

docker-env

docker-uid

docker-memory

docker-swap

docker-cidfile

docker-dns
