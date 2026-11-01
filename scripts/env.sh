# Sourced by the device scripts: load KEY=VALUE lines from ./.env
# (git-ignored; template in .env.example). Variables already set in the
# environment win over the file, so one-off overrides still work:
#   BENCH_QUICK=1 scripts/run_remote.sh
if [ -f .env ]; then
    while IFS= read -r line || [ -n "$line" ]; do
        line=${line%$'\r'}
        case "$line" in ''|'#'*) continue ;; esac
        key=${line%%=*}
        val=${line#*=}
        [[ $key =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || continue
        case "$val" in
            \"*\") val=${val#\"}; val=${val%\"} ;;
            \'*\') val=${val#\'}; val=${val%\'} ;;
        esac
        if [ -z "${!key+x}" ]; then
            export "$key=$val"
        fi
    done < .env
fi
:  # never fail under the callers' set -e
