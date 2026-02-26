################################################################################
#
# components - Component Framework Package
#
################################################################################

COMPONENTS_VERSION = 2.41
COMPONENTS_SITE = ../packages/components
COMPONENTS_SITE_METHOD = local
COMPONENTS_INSTALL_STAGING = YES
COMPONENTS_INSTALL_TARGET = YES
COMPONENTS_AUTORECONF = YES
COMPONENTS_LIBTOOL_PATCH = NO
COMPONENTS_DEPENDENCIES = host-autoconf host-automake host-libtool liburing log4cplus taco-ffmpeg taco-vo taco-pipeline

# 确保 log4cplus 生成静态库（在 Buildroot 中通过环境变量传递）
# Buildroot 会自动处理，但我们需要确保链接时使用静态库
COMPONENTS_CONF_ENV += \
	CPPFLAGS="$(TARGET_CPPFLAGS) -I$(STAGING_DIR)/usr/local/include" \
	LDFLAGS="$(TARGET_LDFLAGS) -L$(STAGING_DIR)/usr/local/lib -Wl,-rpath-link,$(TARGET_DIR)/usr/lib -Wl,-rpath-link,$(TARGET_DIR)/usr/local/lib"

ifeq ($(BR2_ENABLE_DEBUG),y)
COMPONENTS_CONF_OPTS += --enable-debug
endif

define COMPONENTS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 755 $(@D)/qa_cases $(TARGET_DIR)/usr/local/bin
endef

$(eval $(autotools-package))

