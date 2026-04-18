cmd_/home/dharani/OsJack/boilerplate/modules.order := {   echo /home/dharani/OsJack/boilerplate/monitor.ko; :; } | awk '!x[$$0]++' - > /home/dharani/OsJack/boilerplate/modules.order
