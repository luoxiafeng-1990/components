################################################################################
#
# components package
#
################################################################################

COMPONENTS_VERSION = 2.80
COMPONENTS_SITE = ../packages/components
COMPONENTS_SITE_METHOD = local
COMPONENTS_INSTALL_STAGING = YES
COMPONENTS_INSTALL_TARGET = YES
COMPONENTS_AUTORECONF = YES
COMPONENTS_LIBTOOL_PATCH = NO
COMPONENTS_DEPENDENCIES = host-autoconf host-automake host-libtool log4cplus taco-ffmpeg taco-vo taco-pipeline ta-opencv unify-9200O ta-runtime

COMPONENTS_CONF_ENV += \
	LDFLAGS="$(TARGET_LDFLAGS) -L$(STAGING_DIR)/usr/lib -L$(STAGING_DIR)/usr/local/lib -Wl,-rpath-link,$(STAGING_DIR)/usr/lib -Wl,-rpath-link,$(STAGING_DIR)/usr/local/lib -L$(TARGET_DIR)/usr/local/lib -Wl,-rpath-link,$(TARGET_DIR)/usr/lib -Wl,-rpath-link,$(TARGET_DIR)/usr/local/lib" \
	TARGET_DIR=$(TARGET_DIR) \
	STAGING_DIR=$(STAGING_DIR)

COMPONENTS_MAKE_OPTS += TARGET_DIR=$(TARGET_DIR) STAGING_DIR=$(STAGING_DIR)

ifeq ($(BR2_ENABLE_DEBUG),y)
COMPONENTS_CONF_OPTS += --enable-debug
endif

define COMPONENTS_PATCH_CPPFLAGS
	$(SED) 's|^CPPFLAGS = |CPPFLAGS = -I$(STAGING_DIR)/usr/include -I$(STAGING_DIR)/usr/include/opencv4 -I$(STAGING_DIR)/usr/local/include -I$(STAGING_DIR)/usr/local/include/ta-runtime -I$(TARGET_DIR)/usr/local/include -I$(TARGET_DIR)/usr/local/include/opencv4 -I$(TARGET_DIR)/usr/local/include/ta-runtime |' $(@D)/Makefile
	@# Move conflicting FFmpeg 7.x headers out of multilib path to avoid ABI mismatch
	@# GCC's multilib gives $(sysroot)/usr/include/riscv64-linux-gnu highest priority,
	@# which shadows taco-customized FFmpeg 4.2 headers in /usr/local/include/
	@for d in libavformat libavcodec libavutil libswscale libavdevice libavfilter libswresample libpostproc; do \
		if [ -d "$(STAGING_DIR)/usr/include/riscv64-linux-gnu/$$d" ]; then \
			mv "$(STAGING_DIR)/usr/include/riscv64-linux-gnu/$$d" "$(STAGING_DIR)/usr/include/riscv64-linux-gnu/$$d.ffmpeg7.bak"; \
		fi; \
		if [ -d "$(TARGET_DIR)/usr/include/riscv64-linux-gnu/$$d" ]; then \
			mv "$(TARGET_DIR)/usr/include/riscv64-linux-gnu/$$d" "$(TARGET_DIR)/usr/include/riscv64-linux-gnu/$$d.ffmpeg7.bak"; \
		fi; \
	done
endef
COMPONENTS_POST_CONFIGURE_HOOKS += COMPONENTS_PATCH_CPPFLAGS

define COMPONENTS_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) install DESTDIR=$(TARGET_DIR)
	if [ -d $(@D)/webui/dist ]; then \
		mkdir -p $(TARGET_DIR)/usr/local/share/webui; \
		cp -r $(@D)/webui/dist/* $(TARGET_DIR)/usr/local/share/webui/; \
	fi
	mkdir -p /tmp/components/usr/local/bin
	cp -f $(@D)/qa_cases /tmp/components/usr/local/bin
	cp -rf $(@D)/DEBIAN /tmp/components
	dpkg-deb --root-owner-group --build /tmp/components $(TARGET_DIR)/tmp/component_1.0_riscv64.deb
endef

$(eval $(autotools-package))
