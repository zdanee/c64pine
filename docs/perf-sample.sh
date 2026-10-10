#!/bin/sh
# CPU use per program (% of ONE core) every $1 seconds ($2 samples), with x64's state (T = stopped) and memory:
#   docs/perf-sample.sh 5 60
I=${1:-5} N=${2:-60}
snap() {
	for f in /proc/[0-9]*/stat; do cat "$f" 2>/dev/null; echo; done | awk '
		NF { c = $0; n = c; sub(/^[0-9]+ \(/, "", n); sub(/\) .*/, "", n); sub(/^.*\) /, "", c); split(c, f, " ")
		     t[n] += f[12] + f[13]; if (n == "x64") { st = f[1]; xr = f[22] * 4 } if (n == "Xvfb") vr = f[22] * 4
		     if (n == "c64desk") dr += f[22] * 4 }
		END { for (k in t) print k, t[k]; print "@x64", st, xr + 0, vr + 0, dr + 0 }'
	awk '/^cpu /{ print "@cpu", $2 + $3 + $4 + $6 + $7 + $8, $5 }' /proc/stat
}
a=$(snap); k=0; t0=$(date +%s)
echo "time  busy%(of 2 cores)  x64-state x64-RSS-MB Xvfb-MB c64desk-MB MemAvail-MB | top programs (% of one core)"
while [ $k -lt $N ]; do
	sleep "$I"; b=$(snap)
	printf '%s\n--\n%s\n' "$a" "$b" | awk -v I="$I" -v t=$(( $(date +%s) - t0 )) -v avail=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo) '
		/^--$/ { second = 1; next }
		$1 == "@cpu" { if (second) { bu = $2 - cb; id = $3 - ci } else { cb = $2; ci = $3 } next }
		$1 == "@x64" { if (second) { st = $2; xr = $3; vr = $4; dr = $5 } next }
		{ name = $1; for (i = 2; i < NF; i++) name = name "_" $i; v = $NF
		  if (second) d[name] = v - (name in p ? p[name] : v); else p[name] = v }
		END {
			out = ""
			for (k in d) if (d[k] * 100 / (100 * I) >= 0.5) out = out sprintf(" %s=%.1f", k, d[k] / I)
			printf "%4ds busy=%4.1f%% x64=%s %3dMB Xvfb=%dMB desk=%dMB avail=%dMB |%s\n", t, (bu + id) ? 100 * bu / (bu + id) : 0,
			       st == "" ? "-" : st, xr / 1024, vr / 1024, dr / 1024, avail, out }'
	a=$b; k=$((k + 1))
done
