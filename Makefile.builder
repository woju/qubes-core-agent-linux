ifeq ($(PACKAGE_SET),vm)
  RPM_SPEC_FILES := rpm_spec/core-vm.spec
  RPM_SPEC_FILES += rpm_spec/core-vm-doc.spec

  ifneq ($(filter $(DISTRIBUTION), debian qubuntu),)
    DEBIAN_BUILD_DIRS := debian
  endif

  ARCH_BUILD_DIRS := archlinux
endif

# vim: filetype=make
