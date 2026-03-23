################################################################################
#
# components - Component Framework Package
#
################################################################################

COMPONENTS_VERSION = 2.55
COMPONENTS_SITE = ../packages/components
COMPONENTS_SITE_METHOD = local
COMPONENTS_INSTALL_STAGING = YES
COMPONENTS_INSTALL_TARGET = YES
COMPONENTS_AUTORECONF = YES
COMPONENTS_LIBTOOL_PATCH = NO
COMPONENTS_DEPENDENCIES = host-autoconf host-automake host-libtool liburing log4cplus taco-ffmpeg taco-vo taco-pipeline ta-runtime

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
endef

$(eval $(autotools-package))

