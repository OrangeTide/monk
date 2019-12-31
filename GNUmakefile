all ::
clean ::
.PHONY : all clean
CFLAGS := -Wall -W -Os -g
BACKEND ?= x11
####
ARFLAGS=rvU
####
# $1 : tag
# $2 : sources
# $3 : libraries
define genexe
$(eval
EXEC.$1 := $1$X
SRCS.$1 := $2
LIBS.$1 := $3
OBJS.$1 := $$(SRCS.$1:.c=.o)
CFLAGS.$1 ?= $(CFLAGS)
$$(EXEC.$1) : $$(OBJS.$1) $$(LIBS.$1)
all :: $$(EXEC.$1)
clean :: ; $$(RM) $$(EXEC.$1) $$(OBJS.$1)
)
endef
####
# $1 : tag
# $2 : sources
define genlib
$(eval
LIBFILE.$1 := lib$1.a
SRCS.$1 := $2
LIBS.$1 := $3
OBJS.$1 := $$(SRCS.$1:.c=.o)
CFLAGS.$1 ?= $(CFLAGS)
$$(LIBFILE.$1) : $$(LIBFILE.$1)($$(OBJS.$1) $$(LIBS.$1))
all :: $$(LIBFILE.$1)
clean :: ; $$(RM) $$(LIBFILE.$1) $$(OBJS.$1)
)
endef
####
################################################################################
$(call genexe,monk,monk.c,libscreen.a libsystem.a)
$(call genlib,screen,screen.c screen_$(BACKEND).c)
$(call genlib,system,system.c)
################################################################################
DOSPROGS := $(wildcard *.asm)
COMFILES := $(DOSPROGS:.asm=.com)
all :: $(COMFILES)
clean :: ; $(RM) $(COMFILES)
%.com : %.asm
	nasm -o $@ $^
