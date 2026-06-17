CURR_FULL_DIR?=$(shell pwd | sed 's/ /\\ /g')
CURR_FULL_DIR?=$(subst \,/,$(shell cygpath -da .))#M# -m isn't used because it doesn't work on Win98
