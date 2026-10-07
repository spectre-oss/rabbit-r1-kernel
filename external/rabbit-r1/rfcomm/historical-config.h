/* SPDX-License-Identifier: GPL-2.0-only */
/* Reproduce the installed September12 RFCOMM module's compile configuration.
 * The saved reference configuration had these options disabled. This module
 * predates the main kernel's security-network/SELinux configuration change.
 * These overrides apply to this external module only, never the kernel config.
 * They reproduce historical behavior; they are not a security improvement.
 */
#undef CONFIG_SECURITY_NETWORK
#undef CONFIG_NETWORK_SECMARK
#undef CONFIG_SECURITY_SELINUX
