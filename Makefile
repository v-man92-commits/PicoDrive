# DSPicoDrive v0.1 (BlocksDS build): USB disk images + phone/web control.
BLOCKSDS	?= /opt/wonderful/thirdparty/blocksds/core
export BLOCKSDS

NAME		:= DSPicoDrive
GAME_TITLE	:= DSPicoDrive v0.1
GAME_SUBTITLE	:= USB images + phone control
GAME_AUTHOR	:= DSpico
GAME_ICON	:= icon.bmp

ifeq ($(VERBOSE),1)
V		:=
else
V		:= @
endif

ROM		:= $(NAME).nds

.PHONY: all clean arm9 arm7 libtwl tinyusb

all: $(ROM)

# Submodules: tinyusb and libtwl (git submodule update --init).
# TinyUSB gets DSPicoDrive's MSC changes (CD/DVD-ROM emulation); applied once.
tinyusb:
	$(V)if git -C libs/tinyusb apply --check $(CURDIR)/patches/tinyusb-msc-cdrom.patch 2>/dev/null; then \
		echo "  PATCH tinyusb"; git -C libs/tinyusb apply $(CURDIR)/patches/tinyusb-msc-cdrom.patch; fi

# libtwl gets a small diagnostics patch (which ARM7 IRQ/thread is running,
# shown on DSPicoDrive's "Something is stuck" page); applied once.
libtwl:
	$(V)if git -C libs/libtwl apply --check $(CURDIR)/patches/libtwl-irq-breadcrumb.patch 2>/dev/null; then \
		echo "  PATCH libtwl"; git -C libs/libtwl apply $(CURDIR)/patches/libtwl-irq-breadcrumb.patch; fi
	$(V)$(MAKE) -C libs/libtwl -f $(CURDIR)/patches/libtwl-Makefile.blocksds --no-print-directory

arm9: libtwl tinyusb
	$(V)+$(MAKE) -f Makefile.arm9 --no-print-directory

arm7: libtwl tinyusb
	$(V)+$(MAKE) -f Makefile.arm7 --no-print-directory

$(ROM): arm9 arm7
	@echo "  NDSTOOL $@"
	$(V)$(BLOCKSDS)/tools/ndstool/ndstool -c $@ \
		-7 build/arm7.elf -9 build/arm9.elf \
		-b $(GAME_ICON) "$(GAME_TITLE);$(GAME_SUBTITLE);$(GAME_AUTHOR)"

clean:
	$(V)$(MAKE) -f Makefile.arm9 clean --no-print-directory
	$(V)$(MAKE) -f Makefile.arm7 clean --no-print-directory
	$(V)rm -rf $(ROM) build
