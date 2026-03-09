# webserver

---

## Intro

This webserver is a simple webserver which is implemented in c++ and use cmake and conan as build tools.
In addition,it is modified from the tiny-web-server which is also a opensource project.

This project wants to be inited , tested , pacakage managed and built by conan and cmake and designed it into a structure can be divided into several independent modules that can be used as a indepandent libs.

## Mods

1.  mysql
2.  http
3.  https (to be implemented)
4.  lock
5.  threadpool
6.  log
7.  root(root resources)
8.  timer
9.  webserver
10. config parser(to be implemented)

## plans to be done

1. move config info into files (like config.yaml)
2. separate the each mod into a independent lib
3. add test for each mod
4. add the different functions for mysql
.etc
