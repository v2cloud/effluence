FROM ubuntu as zabbix

USER root

RUN apt-get update && \
 apt-get install -y  git build-essential libcurl4-openssl-dev libyaml-dev \
 libcurl4 libyaml-0-2 autotools-dev automake && \
 git clone --branch 6.0.23 https://git.zabbix.com/scm/zbx/zabbix.git --depth 1 /zabbix-source

WORKDIR /zabbix-source

RUN ./bootstrap.sh && ./configure && \

WORKDIR /effluence

RUN  export ZABBIX_SOURCE=/zabbix-source && make

FROM scratch

COPY --from=zabbix /effluence/effluence.so /effluence.so