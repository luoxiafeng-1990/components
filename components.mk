################################################################################
#
# components - Component Framework Package
#
################################################################################

COMPONENTS_VERSION = 1.0
COMPONENTS_SITE = ../packages/components
COMPONENTS_SITE_METHOD = local
COMPONENTS_INSTALL_STAGING = YES
COMPONENTS_INSTALL_TARGET = YES
COMPONENTS_AUTORECONF = YES
COMPONENTS_LIBTOOL_PATCH = NO
COMPONENTS_DEPENDENCIES = host-autoconf host-automake host-libtool liburing log4cplus

# 确保 log4cplus 生成静态库（在 Buildroot 中通过环境变量传递）
# Buildroot 会自动处理，但我们需要确保链接时使用静态库
ifeq ($(BR2_ENABLE_DEBUG),y)
COMPONENTS_CONF_OPTS += --enable-debug
endif

define COMPONENTS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 755 $(@D)/display_test $(TARGET_DIR)/usr/local/bin
endef

$(eval $(autotools-package))

