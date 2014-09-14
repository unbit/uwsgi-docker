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
docker-image = psgi001

[uwsgi]
psgi = /var/www/app.pl
processes = 4
socket = 127.0.0.1:3031
uid = www-data
gid = www-data
```
