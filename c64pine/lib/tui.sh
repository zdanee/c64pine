#
# tui.sh  --  C64 looking dialogs in plain POSIX sh
#
# no dialog/whiptail needed, runs on dash, bash and busybox ash.
# tui_init first, tui_end at the end (also trapped on exit)
#
#   tui_view   TITLE TEXT
#   tui_menu   TITLE TEXT ITEM...          -> TUI_RESULT = number
#   tui_check  TITLE TEXT KEY|LABEL|on|off|fixed ...   -> TUI_RESULT = keys
#   tui_yesno  TITLE TEXT
#   tui_input  TITLE PROMPT DEFAULT [secret]
#   tui_steps  TITLE LOG "LABEL|command"...  stops at the first failure (TUI_FAILED)
#

TUI_TTY=/dev/tty
TUI_NL='
' TUI_CR=$(printf '\r')

tui_colours() {
	e=$(printf '\033')
	if [ "${COLORTERM:-}" = truecolor ] || [ "${COLORTERM:-}" = 24bit ]; then
		TB="${e}[48;2;112;109;235m" TS="${e}[48;2;46;44;155m"
		TF="${e}[38;2;112;109;235m" TR="${e}[38;2;46;44;155m"
		TW="${e}[38;2;255;255;255m"
	elif [ "$(tput colors 2>/dev/null || echo 8)" -ge 256 ] 2>/dev/null; then
		TB="${e}[48;5;105m" TS="${e}[48;5;19m" TF="${e}[38;5;105m" TR="${e}[38;5;19m" TW="${e}[38;5;231m"
	else                                                           # linux console, 16 colours
		TB="${e}[46m" TS="${e}[44m" TF="${e}[1;37m" TR="${e}[0;34m" TW="${e}[1;37m"
	fi
	TN="${e}[0m"
}

tui_size() {
	set -- $(stty size <$TUI_TTY 2>/dev/null)
	TUI_H=${1:-0} TUI_W=${2:-0}
	# can be 0 0 right after the terminal opens
	[ "$TUI_H" -gt 0 ] || TUI_H=${LINES:-24}
	[ "$TUI_W" -gt 0 ] || TUI_W=${COLUMNS:-80}
	[ "$TUI_H" -ge 16 ] || TUI_H=16
	[ "$TUI_W" -ge 50 ] || TUI_W=50
	TUI_X0=4 TUI_Y0=3 TUI_IW=$((TUI_W - 6)) TUI_IH=$((TUI_H - 4))
}

tui_init() {
	if ! { stty -g <$TUI_TTY; } >/dev/null 2>&1; then
		echo "This needs a terminal to talk to you (it found none: run it in a terminal window, not through a pipe)." >&2
		exit 1
	fi
	tui_colours
	TUI_STTY=$(stty -g <$TUI_TTY)
	trap 'tui_end; exit 130' INT TERM
	trap 'tui_end; ${TUI_ATEXIT:-:}' EXIT
	stty -echo -icanon min 1 time 0 <$TUI_TTY
	printf '\033[?1049h\033[?25l' >$TUI_TTY
	tui_size
}

tui_end() {
	[ -n "${TUI_STTY:-}" ] || return 0
	stty "$TUI_STTY" <$TUI_TTY 2>/dev/null
	printf '%s\033[?25h\033[?1049l' "$TN" >$TUI_TTY
	TUI_STTY=""
}


# read a key into TUI_KEY (up, down, enter, esc ... or the char itself)
tui_key() {
	c=$(dd bs=1 count=1 <$TUI_TTY 2>/dev/null; echo x)
	c=${c%x}
	case "$c" in
		"") tui_end; printf '\nNo more input from the terminal: stopped (nothing more was done).\n' >&2; exit 1 ;;
		"$TUI_NL"|"$TUI_CR") TUI_KEY=enter; return ;;
		" ") TUI_KEY=space; return ;;
		"$(printf '\t')") TUI_KEY=tab; return ;;
		"$(printf '\177')"|"$(printf '\010')") TUI_KEY=bs; return ;;
		"$(printf '\033')")
			stty min 0 time 1 <$TUI_TTY
			r=$(dd bs=1 count=3 <$TUI_TTY 2>/dev/null)
			stty min 1 time 0 <$TUI_TTY
			case "$r" in
				"[A"|"OA") TUI_KEY=up ;; "[B"|"OB") TUI_KEY=down ;; "[C"|"OC") TUI_KEY=right ;; "[D"|"OD") TUI_KEY=left ;;
				"[5~") TUI_KEY=pgup ;; "[6~") TUI_KEY=pgdn ;; "[H"|"OH"|"[1~") TUI_KEY=home ;; "[F"|"OF"|"[4~") TUI_KEY=end ;;
				"") TUI_KEY=esc ;; *) TUI_KEY=other ;;
			esac
			return ;;
	esac
	TUI_KEY=$c
}



### drawing ###

tui_at() { printf '\033[%d;%dH' "$1" "$2"; }


tui_frame() {
	title=$1 hints=$2
	tui_size
	{
		printf '%s' "$TB"
		r=1
		while [ $r -le "$TUI_H" ]; do tui_at $r 1; printf '%*s' "$TUI_W" ''; r=$((r + 1)); done
		printf '%s%s' "$TS" "$TF"
		r=2
		while [ $r -lt "$TUI_H" ]; do tui_at $r 3; printf '%*s' $((TUI_W - 4)) ''; r=$((r + 1)); done
		t="**** $title ****"
		tui_at 2 $(( (TUI_W - ${#t}) / 2 + 1 )); printf '%s' "$t"
		hints=$(printf '%s' "$hints" | cut -c1-$((TUI_W - 6)))
		tui_at $((TUI_H - 1)) 3; printf '%s%s %s%*s%s%s' "$TB" "$TR" "$hints" $((TUI_W - 5 - ${#hints})) '' "$TS" "$TF"
	} >$TUI_TTY
}

# ROW TEXT [rev]
tui_line() {
	row=$1 txt=$2
	txt=$(printf '%s' "$txt" | cut -c1-"$TUI_IW")
	if [ "${3:-}" = rev ]; then printf '%s%s%s' "$TB" "$TR" "$(tui_at "$row" $TUI_X0)$txt$(printf '%*s' $((TUI_IW - ${#txt})) '')" >$TUI_TTY
	else printf '%s%s%s%*s' "$TS" "$TF" "$(tui_at "$row" $TUI_X0)$txt" $((TUI_IW - ${#txt})) '' >$TUI_TTY; fi
	printf '%s%s' "$TS" "$TF" >$TUI_TTY
}


tui_wrap() { printf '%s\n' "$1" | fold -s -w "$TUI_IW" >"$TUI_TMP"; }

TUI_TMP=${TMPDIR:-/tmp}/tui.$$
TUI_TMP2=${TMPDIR:-/tmp}/tui2.$$



### widgets ###

tui_msg() {   # no key, just "please wait"
	tui_frame "$1" "PLEASE WAIT"
	tui_wrap "$2"
	i=0
	while IFS= read -r l; do tui_line $((TUI_Y0 + i)) "$l"; i=$((i + 1)); done <"$TUI_TMP"
	rm -f "$TUI_TMP"
}

tui_view() {
	title=$1 body=$2 top=1
	tui_size; tui_wrap "$body"
	n=$(wc -l <"$TUI_TMP")
	page=$((TUI_IH - 3))
	while :; do
		tui_frame "$title" "RETURN = GO ON   ESC = BACK   ARROWS / PGUP / PGDN = SCROLL"
		i=0
		sed -n "${top},$((top + page - 1))p" "$TUI_TMP" | while IFS= read -r l; do tui_line $((TUI_Y0 + i)) "$l"; i=$((i + 1)); done
		[ $n -gt $page ] && tui_line $((TUI_Y0 + page)) "$(printf '%s' "-- $top-$((top + page - 1 < n ? top + page - 1 : n)) OF $n LINES --")"
		tui_key
		case "$TUI_KEY" in
			enter) rm -f "$TUI_TMP"; return 0 ;;
			esc|q) rm -f "$TUI_TMP"; return 1 ;;
			up) [ $top -gt 1 ] && top=$((top - 1)) ;;
			down) [ $((top + page)) -le $n ] && top=$((top + 1)) ;;
			pgup|left) top=$((top - page)); [ $top -lt 1 ] && top=1 ;;
			pgdn|right|space) [ $((top + page)) -le $n ] && top=$((top + page)) ;;
			home) top=1 ;;
			end) top=$((n - page + 1)); [ $top -lt 1 ] && top=1 ;;
		esac
	done
}

tui_menu() {
	title=$1 body=$2; shift 2
	sel=1 cnt=$# off=0
	tui_size; tui_wrap "$body"
	bl=$(wc -l <"$TUI_TMP")
	while :; do
		vis=$((TUI_IH - bl - 4)); [ $vis -ge 2 ] || vis=2
		[ $sel -le $off ] && off=$((sel - 1))
		[ $sel -gt $((off + vis)) ] && off=$((sel - vis))
		tui_frame "$title" "UP / DOWN / PGUP / PGDN = CHOOSE   RETURN = OK   ESC = BACK"
		i=0
		while IFS= read -r l; do tui_line $((TUI_Y0 + i)) "$l"; i=$((i + 1)); done <"$TUI_TMP"
		k=1
		for it; do
			r=$((k - off))
			if [ $r -ge 1 ] && [ $r -le $vis ]; then
				if [ $k -eq $sel ]; then tui_line $((TUI_Y0 + bl + r)) "  $it" rev; else tui_line $((TUI_Y0 + bl + r)) "  $it"; fi
			fi
			k=$((k + 1))
		done
		[ $cnt -gt $vis ] && tui_line $((TUI_Y0 + bl + vis + 1)) "  ($sel OF $cnt)"
		tui_key
		case "$TUI_KEY" in
			up) [ $sel -gt 1 ] && sel=$((sel - 1)) ;;
			down|tab) [ $sel -lt $cnt ] && sel=$((sel + 1)) ;;
			pgup) sel=$((sel - vis)); [ $sel -ge 1 ] || sel=1 ;;
			pgdn) sel=$((sel + vis)); [ $sel -le $cnt ] || sel=$cnt ;;
			home) sel=1 ;;
			end) sel=$cnt ;;
			enter) TUI_RESULT=$sel; rm -f "$TUI_TMP"; return 0 ;;
			esc) rm -f "$TUI_TMP"; return 1 ;;
			[1-9]) [ $cnt -le 9 ] && [ "$TUI_KEY" -le $cnt ] && { TUI_RESULT=$TUI_KEY; rm -f "$TUI_TMP"; return 0; } ;;
		esac
	done
}

tui_yesno() { tui_menu "$1" "$2" "YES" "NO" && [ "$TUI_RESULT" = 1 ]; }


# fixed = always on, can't be unticked
tui_check() {
	title=$1 body=$2; shift 2
	sel=1 cnt=$# off=0
	st=""; for it in "$@"; do st="$st ${it##*|}"; done
	tui_size; tui_wrap "$body"
	bl=$(wc -l <"$TUI_TMP")
	while :; do
		vis=$((TUI_IH - bl - 5)); [ $vis -ge 2 ] || vis=2
		[ $sel -le $off ] && off=$((sel - 1))
		[ $sel -gt $((off + vis)) ] && off=$((sel - vis))
		tui_frame "$title" "UP / DOWN = CHOOSE   SPACE = ON / OFF   RETURN = OK   ESC = BACK"
		i=0
		while IFS= read -r l; do tui_line $((TUI_Y0 + i)) "$l"; i=$((i + 1)); done <"$TUI_TMP"
		k=1
		for it; do
			s=$(echo $st | cut -d' ' -f$k)
			lab=${it#*|}; lab=${lab%|*}
			case "$s" in on) box="[X]" ;; fixed) box="[*]" ;; *) box="[ ]" ;; esac
			r=$((k - off))
			if [ $r -ge 1 ] && [ $r -le $vis ]; then
				if [ $k -eq $sel ]; then tui_line $((TUI_Y0 + bl + r)) "  $box $lab" rev; else tui_line $((TUI_Y0 + bl + r)) "  $box $lab"; fi
			fi
			k=$((k + 1))
		done
		more=""; [ $off -gt 0 ] && more=" (MORE ABOVE)"; [ $((off + vis)) -lt $cnt ] && more="$more (MORE BELOW)"
		tui_line $((TUI_Y0 + bl + (cnt < vis ? cnt : vis) + 2)) "[*] = NEEDED, ALWAYS ON$more"
		tui_key
		case "$TUI_KEY" in
			up) [ $sel -gt 1 ] && sel=$((sel - 1)) ;;
			down|tab) [ $sel -lt $cnt ] && sel=$((sel + 1)) ;;
			space|x)
				s=$(echo $st | cut -d' ' -f$sel)
				case "$s" in on) ns=off ;; off) ns=on ;; *) ns=$s ;; esac
				st=$(echo $st | awk -v k=$sel -v v=$ns '{ $k = v; print }') ;;
			enter)
				TUI_RESULT="" k=1
				for it; do
					s=$(echo $st | cut -d' ' -f$k)
					[ "$s" = off ] || TUI_RESULT="$TUI_RESULT ${it%%|*}"
					k=$((k + 1))
				done
				TUI_RESULT=${TUI_RESULT# }
				rm -f "$TUI_TMP"; return 0 ;;
			esc) rm -f "$TUI_TMP"; return 1 ;;
		esac
	done
}

tui_input() {
	title=$1 prompt=$2 val=${3:-} secret=${4:-}
	while :; do
		tui_frame "$title" "TYPE, RETURN = OK   ESC = BACK"
		tui_line $TUI_Y0 "$prompt"
		if [ -n "$secret" ]; then shown=$(printf '%s' "$val" | sed 's/./*/g'); else shown=$val; fi
		tui_line $((TUI_Y0 + 2)) "$shown"
		printf '%s%s%s ' "$(tui_at $((TUI_Y0 + 2)) $((TUI_X0 + ${#shown})))" "$TB" "" >$TUI_TTY   # the cursor
		printf '%s%s' "$TS" "$TF" >$TUI_TTY
		tui_key
		case "$TUI_KEY" in
			enter) TUI_RESULT=$val; return 0 ;;
			esc) return 1 ;;
			bs) val=${val%?} ;;
			space) val="$val " ;;
			up|down|left|right|pgup|pgdn|home|end|tab|other) ;;
			*) val="$val$TUI_KEY" ;;
		esac
	done
}


tui_steps() {
	title=$1 log=$2; shift 2
	: >"$log"
	n=$# k=1 failed=""
	for step; do
		lab=${step%%|*} cmd=${step#*|}
		tui_steps_draw "$title" "$log" "$k" "$@"
		echo "==== $lab" >>"$log"
		( eval "$cmd" ) >>"$log" 2>&1 </dev/null &
		pid=$!
		while kill -0 $pid 2>/dev/null; do
			sleep 1
			tui_steps_draw "$title" "$log" "$k" "$@"
		done
		if ! wait $pid; then TUI_FAILED=$lab; tui_steps_draw "$title" "$log" "$k" "$@"; return 1; fi
		k=$((k + 1))
	done
	tui_steps_draw "$title" "$log" "$k" "$@"
	return 0
}

tui_steps_draw() {   # _vars: called inside tui_steps' loop
	_t=$1 _lg=$2 _cur=$3; shift 3
	tui_frame "$_t" "PLEASE WAIT (THE LOG: $_lg)"
	_k=1
	for _s; do
		_lb=${_s%%|*}
		if [ $_k -lt $_cur ]; then _m="OK     "; elif [ $_k -eq $_cur ]; then _m="LOADING"; else _m="       "; fi
		[ -n "${TUI_FAILED:-}" ] && [ $_k -eq $_cur ] && _m="?ERROR "
		tui_line $((TUI_Y0 + _k - 1)) " $_m $_lb"
		_k=$((_k + 1))
	done
	_room=$((TUI_IH - $# - 3))
	[ $_room -gt 0 ] || return 0
	tui_line $((TUI_Y0 + $# + 1)) "$(printf '%*s' "$TUI_IW" '' | tr ' ' '-')"
	_i=0
	tail -n $_room "$_lg" 2>/dev/null | tr -d '\r' | cut -c1-"$TUI_IW" | while IFS= read -r _l; do
		tui_line $((TUI_Y0 + $# + 2 + _i)) "$_l"; _i=$((_i + 1))
	done
}
