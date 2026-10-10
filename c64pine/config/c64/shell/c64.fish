# C64PINE terminals with fish (c64-shell): the C64 prompt over your own config
function fish_prompt; printf '\e]0;%s\a\e[1 q' (prompt_pwd); echo READY.; end
function fish_right_prompt; end
function fish_greeting; c64-banner; end
alias sys vfetch; alias la 'ls -la'; alias list 'ls -l'; alias cls clear
