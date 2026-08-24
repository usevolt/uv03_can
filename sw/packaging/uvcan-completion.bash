# bash completion for uvcan
#
# Installed by install.sh as
# <data dir>/bash-completion/completions/uvcan, i.e. loaded automatically by
# the bash-completion package when a new shell first completes 'uvcan'.
#
# The option names are not written down here: they are read from
# 'uvcan --help', which lists one per line, so this follows whatever the
# installed binary supports and never has to be updated when a command is
# added. The list is read once per shell and kept in a variable.
#
# The only hand-kept part is _uvcan_path_opts below, the options whose value is
# a file or a directory. An option missing from it still completes as a file,
# which is the default for everything with an argument.

# Options whose value is a path. Anything not listed also gets file completion,
# so this list is about intent, not correctness.
_uvcan_path_opts="--db --dev --sys --firmware --linuxbin --bootloader --media
--makeuvdev --loadbin --loadbinwfr --segloadbin --segloadbinwfr --uvloadbin
--uvloadbinwfr --loadparam --saveparam --saveparamall --loadmedia --exportc
--exporth --export --srcdest --incdest"

# All options the installed uvcan knows, read from its help once per shell.
_uvcan_all_opts() {
	if [ -z "$_UVCAN_OPTS" ]; then
		_UVCAN_OPTS=$(uvcan --help 2>/dev/null |
				sed -n 's/^\(--[a-zA-Z0-9_-]\{1,\}\).*/\1/p')
	fi
	printf '%s\n' "$_UVCAN_OPTS"
}

_uvcan() {
	local cur prev
	cur="${COMP_WORDS[COMP_CWORD]}"
	prev="${COMP_WORDS[COMP_CWORD-1]}"

	# a value for the option before the cursor
	case " $_uvcan_path_opts " in
		*" $prev "*)
			COMPREPLY=($(compgen -f -- "$cur"))
			compopt -o filenames 2>/dev/null
			return ;;
	esac
	case "$prev" in
		--can|-c)
			# the network interfaces of this machine, CAN or not: the CAN ones
			# are what uvcan wants and the rest are obvious enough to skip
			COMPREPLY=($(compgen -W "$(ls /sys/class/net 2>/dev/null)" -- "$cur"))
			return ;;
		--baud|-b)
			COMPREPLY=($(compgen -W "125000 250000 500000 1000000" -- "$cur"))
			return ;;
		--nodeid|-n|--forcenodeid)
			# node ids are hexadecimal; 0x7f is the bootloader's
			COMPREPLY=($(compgen -W "0x7f" -- "$cur"))
			return ;;
	esac

	if [ "${cur:0:1}" = "-" ]; then
		COMPREPLY=($(compgen -W "$(_uvcan_all_opts)" -- "$cur"))
	else
		COMPREPLY=($(compgen -f -- "$cur"))
		compopt -o filenames 2>/dev/null
	fi
}

complete -F _uvcan uvcan
