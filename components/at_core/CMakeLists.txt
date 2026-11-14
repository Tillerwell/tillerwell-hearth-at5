idf_component_register(SRCS "at_core.c" "at_ctl.c" "at_learn.c" "at_cooldown.c" "at_coast.c" "at_runlog.c" "at_warm.c" "at_early.c" "at_fuel.c" "at_battery.c" "at_timers.c" "at_tz.c" "at_version.c"
                       INCLUDE_DIRS "include"
                       REQUIRES at_proto)
