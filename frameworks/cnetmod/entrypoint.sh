#!/bin/bash
set -eu

workers="$(nproc)"
CNETMOD_QUIC_GSO="${CNETMOD_QUIC_GSO:-1}"
export CNETMOD_QUIC_GSO
mode="${CNETMOD_MODE:-server}"

if [ "${mode}" = "gateway-h3" ]; then
    exec /gateway-h3 /certs/server.crt /certs/server.key 8443 "${workers}" \
        /data/static "${CNETMOD_GATEWAY_UPSTREAM:-http://127.0.0.1:8085}"
fi

if [ "${mode}" = "backend" ]; then
    exec /server
fi

if [ -r /certs/server.crt ] && [ -r /certs/server.key ]; then
    /server-h3 /certs/server.crt /certs/server.key 8443 "${workers}" /data/static &
    h3_pid="$!"
else
    h3_pid=""
fi
/server &
server_pid="$!"

terminate() {
    [ -z "${h3_pid}" ] || kill "${h3_pid}" 2>/dev/null || true
    kill "${server_pid}" 2>/dev/null || true
}
trap terminate INT TERM EXIT

set +e
if [ -n "${h3_pid}" ]; then
    wait -n "${h3_pid}" "${server_pid}"
else
    wait "${server_pid}"
fi
status="$?"
set -e
terminate
[ -z "${h3_pid}" ] || wait "${h3_pid}" 2>/dev/null || true
wait "${server_pid}" 2>/dev/null || true
exit "${status}"
