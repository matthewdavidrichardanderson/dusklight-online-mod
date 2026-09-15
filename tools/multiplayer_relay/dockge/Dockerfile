FROM ubuntu:24.04

ARG RELAY_VERSION

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        curl \
        jq \
        libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --no-create-home relay

COPY dusklight_online_relay /usr/local/bin/dusklight_online_relay
COPY run-with-gluetun.sh /usr/local/bin/run-with-gluetun

RUN chmod 0755 /usr/local/bin/dusklight_online_relay /usr/local/bin/run-with-gluetun \
    && test -n "$RELAY_VERSION" \
    && test "$(/usr/local/bin/dusklight_online_relay --version)" = \
        "dusklight_online_relay ${RELAY_VERSION#v}"

USER relay
ENTRYPOINT ["/usr/local/bin/run-with-gluetun"]

