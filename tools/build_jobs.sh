#!/usr/bin/env bash
# Pick useful native build parallelism without exhausting memory on Stan Math
# translation units. Override with STANLI_JOBS (or CMAKE_BUILD_PARALLEL_LEVEL).

_stanli_cap_jobs_by_memory() {
  local detected_cores=$1 detected_memory=$2 per_job=$3 memory_jobs=4 jobs
  if [[ "$detected_memory" =~ ^[1-9][0-9]*$ ]]; then
    memory_jobs=$((detected_memory / per_job))
    ((memory_jobs > 0)) || memory_jobs=1
  fi
  jobs=$detected_cores
  ((memory_jobs < jobs)) && jobs=$memory_jobs
  printf '%s\n' "$jobs"
}

# Bash arithmetic is signed. Check a decimal limit as text before evaluating
# it so cgroup-v1's UINT64_MAX "unlimited" sentinel cannot wrap to -1.
_stanli_limit_below_one_pib() {
  local value=$1 ceiling=1125899906842624
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || return 1
  ((${#value} < ${#ceiling})) ||
    { ((${#value} == ${#ceiling})) && ((value < ceiling)); }
}

_stanli_smaller_memory_limit() {
  local current=$1 candidate=$2
  if _stanli_limit_below_one_pib "$candidate" &&
     { ! _stanli_limit_below_one_pib "$current" ||
       ((candidate < current)); }; then
    printf '%s\n' "$candidate"
  else
    printf '%s\n' "$current"
  fi
}

_stanli_memory_limit_in_tree() {
  local root=$1 relative=$2 filename=$3 cursor value result=""
  [[ "$relative" == /* ]] || relative="/$relative"
  cursor="${root}${relative%/}"
  [[ "$cursor" == "$root"* ]] || return
  while :; do
    if [[ -r "$cursor/$filename" ]]; then
      value=$(< "$cursor/$filename")
      result=$(_stanli_smaller_memory_limit "$result" "$value")
    fi
    [[ "$cursor" == "$root" ]] && break
    cursor=${cursor%/*}
    [[ "$cursor" == "$root"* ]] || break
  done
  printf '%s\n' "$result"
}

_stanli_linux_cgroup_memory_limit() {
  local relative result="" candidate
  if [[ -r /proc/self/cgroup ]]; then
    relative=$(awk -F: '$1 == "0" { print $3; exit }' /proc/self/cgroup)
    if [[ -n "$relative" && -d /sys/fs/cgroup ]]; then
      candidate=$(_stanli_memory_limit_in_tree \
        /sys/fs/cgroup "$relative" memory.max)
      result=$(_stanli_smaller_memory_limit "$result" "$candidate")
    fi
    relative=$(awk -F: '$2 ~ /(^|,)memory(,|$)/ { print $3; exit }' \
      /proc/self/cgroup)
    if [[ -n "$relative" && -d /sys/fs/cgroup/memory ]]; then
      candidate=$(_stanli_memory_limit_in_tree \
        /sys/fs/cgroup/memory "$relative" memory.limit_in_bytes)
      result=$(_stanli_smaller_memory_limit "$result" "$candidate")
    fi
  fi
  # Also cover cgroup namespaces where /proc/self/cgroup reports `/`.
  if [[ -r /sys/fs/cgroup/memory.max ]]; then
    candidate=$(< /sys/fs/cgroup/memory.max)
    result=$(_stanli_smaller_memory_limit "$result" "$candidate")
  elif [[ -r /sys/fs/cgroup/memory/memory.limit_in_bytes ]]; then
    candidate=$(< /sys/fs/cgroup/memory/memory.limit_in_bytes)
    result=$(_stanli_smaller_memory_limit "$result" "$candidate")
  fi
  printf '%s\n' "$result"
}

_stanli_cpu_quota_in_tree() {
  local root=$1 relative=$2 mode=$3 cursor quota period quota_jobs result=""
  [[ "$relative" == /* ]] || relative="/$relative"
  cursor="${root}${relative%/}"
  [[ "$cursor" == "$root"* ]] || return
  while :; do
    quota=""
    period=""
    if [[ "$mode" = v2 && -r "$cursor/cpu.max" ]]; then
      read -r quota period < "$cursor/cpu.max" || true
    elif [[ "$mode" = v1 &&
            -r "$cursor/cpu.cfs_quota_us" &&
            -r "$cursor/cpu.cfs_period_us" ]]; then
      quota=$(< "$cursor/cpu.cfs_quota_us")
      period=$(< "$cursor/cpu.cfs_period_us")
    fi
    if _stanli_limit_below_one_pib "$quota" &&
       _stanli_limit_below_one_pib "$period"; then
      quota_jobs=$(((quota + period - 1) / period))
      ((quota_jobs > 0)) || quota_jobs=1
      if [[ ! "$result" =~ ^[1-9][0-9]*$ ]] ||
         ((quota_jobs < result)); then
        result=$quota_jobs
      fi
    fi
    [[ "$cursor" == "$root" ]] && break
    cursor=${cursor%/*}
    [[ "$cursor" == "$root"* ]] || break
  done
  printf '%s\n' "$result"
}

_stanli_linux_cgroup_cpu_jobs() {
  local relative result="" candidate root
  [[ -r /proc/self/cgroup ]] || return
  relative=$(awk -F: '$1 == "0" { print $3; exit }' /proc/self/cgroup)
  if [[ -n "$relative" && -d /sys/fs/cgroup ]]; then
    result=$(_stanli_cpu_quota_in_tree /sys/fs/cgroup "$relative" v2)
  fi
  relative=$(awk -F: '$2 ~ /(^|,)cpu(,|$)/ { print $3; exit }' \
    /proc/self/cgroup)
  for root in /sys/fs/cgroup/cpu /sys/fs/cgroup/cpu,cpuacct; do
    if [[ -n "$relative" && -d "$root" ]]; then
      candidate=$(_stanli_cpu_quota_in_tree "$root" "$relative" v1)
      if [[ "$candidate" =~ ^[1-9][0-9]*$ ]] &&
         { [[ ! "$result" =~ ^[1-9][0-9]*$ ]] ||
           ((candidate < result)); }; then
        result=$candidate
      fi
    fi
  done
  printf '%s\n' "$result"
}

stanli_detect_build_jobs() {
  local requested cores quota_jobs memory_bytes cgroup_limit bytes_per_job job_memory_gib os
  requested=${STANLI_JOBS:-${CMAKE_BUILD_PARALLEL_LEVEL:-}}
  if [[ -n "$requested" ]]; then
    if [[ ! "$requested" =~ ^[1-9][0-9]*$ ]]; then
      echo "STANLI_JOBS must be a positive integer (got '$requested')" >&2
      return 2
    fi
    printf '%s\n' "$requested"
    return
  fi

  os=$(uname -s 2>/dev/null || true)
  cores=""
  # Coreutils nproc honors Linux CPU affinity/cpuset limits; getconf can
  # report every host CPU from inside a constrained container.
  if [[ "$os" = Linux ]] && command -v nproc >/dev/null 2>&1; then
    cores=$(nproc 2>/dev/null || true)
  fi
  if command -v getconf >/dev/null 2>&1; then
    [[ "$cores" =~ ^[1-9][0-9]*$ ]] ||
      cores=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
  fi
  if [[ ! "$cores" =~ ^[1-9][0-9]*$ ]] &&
     command -v sysctl >/dev/null 2>&1; then
    cores=$(sysctl -n hw.logicalcpu 2>/dev/null || true)
  fi
  if [[ ! "$cores" =~ ^[1-9][0-9]*$ ]] &&
     [[ ${NUMBER_OF_PROCESSORS:-} =~ ^[1-9][0-9]*$ ]]; then
    cores=$NUMBER_OF_PROCESSORS
  fi
  [[ "$cores" =~ ^[1-9][0-9]*$ ]] || cores=1
  if [[ "$os" = Linux ]]; then
    quota_jobs=$(_stanli_linux_cgroup_cpu_jobs)
    if [[ "$quota_jobs" =~ ^[1-9][0-9]*$ ]] &&
       ((quota_jobs < cores)); then
      cores=$quota_jobs
    fi
  fi

  memory_bytes=""
  # Apple clang peaked at 2.75 GiB in the clean-build benchmark. Linux GCC's
  # largest native shard has reached about 3.9 GiB, so Linux/unknown hosts get
  # a wider safety margin. Windows CI has validated four jobs in 16 GiB.
  bytes_per_job=6442450944
  case "$os" in
    Darwin)
      bytes_per_job=4294967296
      memory_bytes=$(sysctl -n hw.memsize 2>/dev/null || true)
      ;;
    Linux)
      if [[ -r /proc/meminfo ]]; then
        memory_bytes=$(awk '/^MemTotal:/ { printf "%.0f", $2 * 1024; exit }' \
          /proc/meminfo)
      fi
      # Containers can expose the host's MemTotal while enforcing a smaller
      # limit in a nested v1/v2 cgroup. Walk the process cgroup and its parents.
      cgroup_limit=$(_stanli_linux_cgroup_memory_limit)
      memory_bytes=$(_stanli_smaller_memory_limit \
        "$memory_bytes" "$cgroup_limit")
      ;;
    FreeBSD)
      memory_bytes=$(sysctl -n hw.physmem 2>/dev/null || true)
      ;;
    MINGW*|MSYS*|CYGWIN*)
      bytes_per_job=4294967296
      if command -v powershell.exe >/dev/null 2>&1; then
        memory_bytes=$(powershell.exe -NoProfile -Command \
          '(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory' \
          2>/dev/null | tr -d '\r')
      fi
      ;;
  esac

  job_memory_gib=${STANLI_JOB_MEMORY_GIB:-}
  if [[ -n "$job_memory_gib" ]]; then
    if [[ ! "$job_memory_gib" =~ ^[1-9][0-9]*$ ]] ||
       ((${#job_memory_gib} > 4)) || ((job_memory_gib > 1024)); then
      echo "STANLI_JOB_MEMORY_GIB must be an integer from 1 to 1024" \
           "(got '$job_memory_gib')" >&2
      return 2
    fi
    bytes_per_job=$((job_memory_gib * 1073741824))
  fi

  _stanli_cap_jobs_by_memory "$cores" "$memory_bytes" "$bytes_per_job"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
  stanli_detect_build_jobs
fi
