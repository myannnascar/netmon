include $(TOPDIR)/rules.mk

PKG_NAME:=netmon-core
PKG_VERSION:=1.1.0
PKG_RELEASE:=1

PKG_MAINTAINER:=MYAN
PKG_LICENSE:=GPL-3.0

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/netmon-core
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Lightweight Network Monitor Core
  DEPENDS:=+libubox +libubus +libjson-c +libnftnl +libmnl +libsqlite3 +libblobmsg-json
endef

define Package/netmon-core/description
  A lightweight network traffic monitor using nftables.
endef

define Package/netmon-core/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_DIR) $(1)/etc/netmon
	$(INSTALL_DIR) $(1)/lib/upgrade/keep.d
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/netmon $(1)/usr/bin/netmon
	$(INSTALL_BIN) ./files/netmon.init $(1)/etc/init.d/netmon
	$(INSTALL_CONF) ./files/netmon.config $(1)/etc/config/netmon
	$(INSTALL_DATA) ./files/netmon.keep $(1)/lib/upgrade/keep.d/netmon-core
	$(INSTALL_DATA) ./files/netmon.acl.json $(1)/usr/share/rpcd/acl.d/netmon.json
endef

$(eval $(call BuildPackage,netmon-core))
