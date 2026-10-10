# C64PINE terminals with zsh: your ~/.zshrc first, then the C64 prompt (c64-shell)
[ -f "$HOME/.zshrc" ] && { ZDOTDIR=$HOME; source "$HOME/.zshrc"; }
print -n '\e[1 q'
PROMPT=$'READY.\n'
RPROMPT=
precmd() { print -Pn '\e]0;%~\a'; }
alias sys='vfetch' la='ls -la' list='ls -l' cls='clear'
c64-banner
