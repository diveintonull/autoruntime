#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" != "--inner" ]]; then
  exec unshare -Urn -- bash "$0" --inner "$@"
fi
shift

binary=""
output_root=""
minimum_duration_ms=5000
warmup_ms=2000
phase_ms=700
domain_id=73

while (( $# > 0 )); do
  case "$1" in
    --binary)
      binary="$2"
      shift 2
      ;;
    --output-dir)
      output_root="$2"
      shift 2
      ;;
    --minimum-duration-ms)
      minimum_duration_ms="$2"
      shift 2
      ;;
    --warmup-ms)
      warmup_ms="$2"
      shift 2
      ;;
    --phase-ms)
      phase_ms="$2"
      shift 2
      ;;
    --domain-id)
      domain_id="$2"
      shift 2
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [[ ! -r /proc/uptime ||
      ! -x "$binary" || -z "$output_root" ||
      ! "$minimum_duration_ms" =~ ^[0-9]+$ ||
      ! "$warmup_ms" =~ ^[0-9]+$ ||
      ! "$phase_ms" =~ ^[0-9]+$ ||
      ! "$domain_id" =~ ^[0-9]+$ ||
      "$minimum_duration_ms" -eq 0 ||
      "$warmup_ms" -eq 0 || "$phase_ms" -eq 0 ]]; then
  echo "invalid cross-machine orchestration arguments" >&2
  exit 2
fi

binary="$(readlink -f "$binary")"
mkdir -p "$output_root"
output_root="$(readlink -f "$output_root")"
run_directory="$output_root/run-$(date -u +%Y%m%dT%H%M%SZ)-$BASHPID"
mkdir -p "$run_directory"
orchestrator_log="$run_directory/orchestrator.jsonl"

machine_a_holder=""
machine_b_holder=""
machine_a_process=""
machine_b_process=""
generation=1

cleanup() {
  set +e
  if [[ -n "${machine_a_process:-}" ]]; then
    kill -TERM "$machine_a_process" 2>/dev/null
    wait "$machine_a_process" 2>/dev/null
  fi
  if [[ -n "${machine_b_process:-}" ]]; then
    kill -TERM "$machine_b_process" 2>/dev/null
    wait "$machine_b_process" 2>/dev/null
  fi
  if [[ -n "${machine_a_holder:-}" ]]; then
    kill -TERM "$machine_a_holder" 2>/dev/null
    wait "$machine_a_holder" 2>/dev/null
  fi
  if [[ -n "${machine_b_holder:-}" ]]; then
    kill -TERM "$machine_b_holder" 2>/dev/null
    wait "$machine_b_holder" 2>/dev/null
  fi
}
trap cleanup EXIT INT TERM

sleep_milliseconds() {
  local milliseconds="$1"
  local seconds
  printf -v seconds '%d.%03d' \
    "$((milliseconds / 1000))" \
    "$((milliseconds % 1000))"
  sleep "$seconds"
}

monotonic_milliseconds() {
  awk '{printf "%.0f\n", $1 * 1000}' /proc/uptime
}

log_phase() {
  local phase="$1"
  local monotonic_ms
  monotonic_ms="$(monotonic_milliseconds)"
  printf \
    '{"schema_version":1,"record_type":"phase","phase":"%s","generation":%d,"monotonic_ms":%s,"wall_time_ns":%s}\n' \
    "$phase" "$generation" "$monotonic_ms" "$(date +%s%N)" >>"$orchestrator_log"
}

wait_ready() {
  local process_id="$1"
  local output_file="$2"
  local attempt
  for attempt in $(seq 1 150); do
    if grep -q '"record_type":"ready"' "$output_file" 2>/dev/null; then
      return 0
    fi
    if ! kill -0 "$process_id" 2>/dev/null; then
      echo "role exited before ready: $output_file" >&2
      return 1
    fi
    sleep 0.1
  done
  echo "role readiness timed out: $output_file" >&2
  return 1
}

dds_uri_a='<CycloneDDS xmlns="https://cdds.io/config"><Domain Id="any"><General><Interfaces><NetworkInterface address="10.88.0.2"/></Interfaces><AllowMulticast>false</AllowMulticast></General><Discovery><Peers><Peer Address="10.88.0.3"/></Peers></Discovery></Domain></CycloneDDS>'
dds_uri_b='<CycloneDDS xmlns="https://cdds.io/config"><Domain Id="any"><General><Interfaces><NetworkInterface address="10.88.0.3"/></Interfaces><AllowMulticast>false</AllowMulticast></General><Discovery><Peers><Peer Address="10.88.0.2"/></Peers></Discovery></Domain></CycloneDDS>'

start_machine_a() {
  local output_file="$run_directory/machine-a-generation-$generation.jsonl"
  nsenter -t "$machine_a_holder" -n \
    env CYCLONEDDS_URI="$dds_uri_a" \
    "$binary" \
      --role machine-a \
      --address 10.88.0.2 \
      --peer-address 10.88.0.3 \
      --discovery-port 41002 \
      --peer-discovery-port 41003 \
      --rpc-port 42002 \
      --domain-id "$domain_id" \
      --generation "$generation" \
      --publish-period-ms 20 \
      --heartbeat-ms 50 \
      --lease-ms 300 \
      --output "$output_file" &
  machine_a_process=$!
  wait_ready "$machine_a_process" "$output_file"
}

start_machine_b() {
  local output_file="$run_directory/machine-b.jsonl"
  nsenter -t "$machine_b_holder" -n \
    env CYCLONEDDS_URI="$dds_uri_b" \
    "$binary" \
      --role machine-b \
      --address 10.88.0.3 \
      --peer-address 10.88.0.2 \
      --discovery-port 41003 \
      --peer-discovery-port 41002 \
      --rpc-port 42003 \
      --domain-id "$domain_id" \
      --generation 1 \
      --probe-period-ms 50 \
      --rpc-timeout-ms 150 \
      --dds-stall-ms 300 \
      --heartbeat-ms 50 \
      --lease-ms 300 \
      --latency-window 512 \
      --require-faults \
      --output "$output_file" &
  machine_b_process=$!
  wait_ready "$machine_b_process" "$output_file"
}

printf \
  '{"schema_version":1,"record_type":"environment","runner":"autoruntime_cross_machine_orchestrator","topology":"two_rootless_network_namespaces","duration_clock":"proc_uptime_monotonic_ms","machine_a":"10.88.0.2","machine_b":"10.88.0.3","minimum_duration_ms":%d,"warmup_ms":%d,"phase_ms":%d,"domain_id":%d}\n' \
  "$minimum_duration_ms" "$warmup_ms" "$phase_ms" "$domain_id" \
  >"$orchestrator_log"

ip link set lo up
unshare -n sh -c 'ip link set lo up; exec sleep 2147483647' &
machine_a_holder=$!
unshare -n sh -c 'ip link set lo up; exec sleep 2147483647' &
machine_b_holder=$!

ip link add ar-br0 type bridge
ip link set ar-br0 up
ip link add ar-a0 type veth peer name ar-a-br
ip link add ar-b0 type veth peer name ar-b-br
ip link set ar-a0 netns "$machine_a_holder"
ip link set ar-b0 netns "$machine_b_holder"
ip link set ar-a-br master ar-br0
ip link set ar-b-br master ar-br0
ip link set ar-a-br up
ip link set ar-b-br up
nsenter -t "$machine_a_holder" -n ip link set ar-a0 up
nsenter -t "$machine_a_holder" -n ip addr add 10.88.0.2/24 dev ar-a0
nsenter -t "$machine_b_holder" -n ip link set ar-b0 up
nsenter -t "$machine_b_holder" -n ip addr add 10.88.0.3/24 dev ar-b0

start_machine_b
start_machine_a
log_phase warmup
sleep_milliseconds "$warmup_ms"

fault_started_ms="$(monotonic_milliseconds)"
while kill -0 "$machine_b_process" 2>/dev/null; do
  log_phase network_delay
  nsenter -t "$machine_a_holder" -n \
    tc qdisc replace dev ar-a0 root netem delay 20ms
  nsenter -t "$machine_b_holder" -n \
    tc qdisc replace dev ar-b0 root netem delay 20ms
  sleep_milliseconds "$phase_ms"
  nsenter -t "$machine_a_holder" -n \
    tc qdisc del dev ar-a0 root 2>/dev/null || true
  nsenter -t "$machine_b_holder" -n \
    tc qdisc del dev ar-b0 root 2>/dev/null || true

  log_phase network_disconnect
  ip link set ar-a-br down
  sleep_milliseconds "$phase_ms"

  ip link set ar-a-br up
  log_phase network_reconnect
  sleep_milliseconds "$phase_ms"

  log_phase dds_peer_disappear
  kill -KILL "$machine_a_process"
  wait "$machine_a_process" 2>/dev/null || true
  machine_a_process=""
  sleep_milliseconds "$phase_ms"

  generation=$((generation + 1))
  start_machine_a
  log_phase peer_restart
  sleep_milliseconds "$warmup_ms"

  current_ms="$(monotonic_milliseconds)"
  elapsed_ms=$((current_ms - fault_started_ms))
  if (( elapsed_ms >= minimum_duration_ms )); then
    break
  fi
done
actual_fault_duration_ms=$(( $(monotonic_milliseconds) - fault_started_ms ))

log_phase final_verification
sleep_milliseconds "$warmup_ms"

kill -TERM "$machine_b_process"
set +e
wait "$machine_b_process"
machine_b_status=$?
set -e
machine_b_process=""

kill -TERM "$machine_a_process"
set +e
wait "$machine_a_process"
machine_a_status=$?
set -e
machine_a_process=""

if (( machine_a_status == 0 && machine_b_status == 0 &&
      actual_fault_duration_ms >= minimum_duration_ms )); then
  final_status="passed"
  final_exit=0
else
  final_status="failed"
  final_exit=1
fi
printf \
  '{"schema_version":1,"record_type":"summary","status":"%s","machine_a_exit":%d,"machine_b_exit":%d,"requested_fault_duration_ms":%d,"actual_fault_duration_ms":%d,"run_directory":"%s"}\n' \
  "$final_status" "$machine_a_status" "$machine_b_status" \
  "$minimum_duration_ms" "$actual_fault_duration_ms" \
  "$run_directory" >>"$orchestrator_log"

echo "cross_machine_run_directory=$run_directory"
exit "$final_exit"
