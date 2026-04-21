################################################################################
#
# components - Component Framework Package
#
################################################################################

COMPONENTS_VERSION = 2.73
COMPONENTS_SITE = ../packages/components
COMPONENTS_SITE_METHOD = local
COMPONENTS_INSTALL_STAGING = YES
COMPONENTS_INSTALL_TARGET = YES
COMPONENTS_AUTORECONF = YES
COMPONENTS_LIBTOOL_PATCH = NO
COMPONENTS_DEPENDENCIES = host-autoconf host-automake host-libtool log4cplus taco-ffmpeg taco-vo taco-pipeline opencv unify-9200O ta-runtime

COMPONENTS_CONF_ENV += \
	LDFLAGS="$(TARGET_LDFLAGS) -L$(STAGING_DIR)/usr/local/lib -Wl,-rpath-link,$(TARGET_DIR)/usr/lib -Wl,-rpath-link,$(TARGET_DIR)/usr/local/lib"

ifeq ($(BR2_ENABLE_DEBUG),y)
COMPONENTS_CONF_OPTS += --enable-debug
endif

define COMPONENTS_PATCH_CPPFLAGS
	$(SED) 's|^CPPFLAGS = |CPPFLAGS = -I$(TARGET_DIR)/usr/local/include/opencv4 -I$(TARGET_DIR)/usr/local/include/ta-runtime |' $(@D)/Makefile
endef
COMPONENTS_POST_CONFIGURE_HOOKS += COMPONENTS_PATCH_CPPFLAGS

define COMPONENTS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 755 $(@D)/qa_cases $(TARGET_DIR)/usr/local/bin
	$(INSTALL) -D -m 755 $(@D)/webui_server $(TARGET_DIR)/usr/local/bin
	@if [ -d "$(@D)/webui/dist" ]; then \
		mkdir -p $(TARGET_DIR)/usr/local/share/webui; \
		cp -r $(@D)/webui/dist/* $(TARGET_DIR)/usr/local/share/webui/; \
	fi
endef

$(eval $(autotools-package))

