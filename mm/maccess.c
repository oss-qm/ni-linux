/*
 * Access kernel memory without faulting.
 */
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/syscalls.h>

/**
 * probe_kernel_read(): safely attempt to read from a location
 * @dst: pointer to the buffer that shall take the data
 * @src: address to read from
 * @size: size of the data chunk
 *
 * Safely read from address @src to the buffer at @dst.  If a kernel fault
 * happens, handle that and return -EFAULT.
 */

long __weak probe_kernel_read(void *dst, const void *src, size_t size)
    __attribute__((alias("__probe_kernel_read")));

long __probe_kernel_read(void *dst, const void *src, size_t size)
{
	long ret;
	mm_segment_t old_fs = get_fs();

	set_fs(KERNEL_DS);
	pagefault_disable();
	ret = __copy_from_user_inatomic(dst,
			(__force const void __user *)src, size);
	pagefault_enable();
	set_fs(old_fs);

	return ret ? -EFAULT : 0;
}
EXPORT_SYMBOL_GPL(probe_kernel_read);

/**
 * probe_kernel_write(): safely attempt to write to a location
 * @dst: address to write to
 * @src: pointer to the data that shall be written
 * @size: size of the data chunk
 *
 * Safely write to address @dst from the buffer at @src.  If a kernel fault
 * happens, handle that and return -EFAULT.
 */
long __weak probe_kernel_write(void *dst, const void *src, size_t size)
    __attribute__((alias("__probe_kernel_write")));

long __probe_kernel_write(void *dst, const void *src, size_t size)
{
	long ret;
	mm_segment_t old_fs = get_fs();

	set_fs(KERNEL_DS);
	pagefault_disable();
	ret = __copy_to_user_inatomic((__force void __user *)dst, src, size);
	pagefault_enable();
	set_fs(old_fs);

	return ret ? -EFAULT : 0;
}

/*
 * Safely copy 'len' bytes from user space 'src' to user space 'dst'.
 * 'len' must be less than or equal to 64. In particular, safely here
 * means that if we are trying to copy memory that has been freed and
 * unmapped we don't crash.
 *
 * Returns
 *    0      copy completed successfully
 *
 *    EFAULT if either the source or destination blocks are not
 *           valid
 *
 *    EINVAL len is greater than 64
 *
 */
SYSCALL_DEFINE3(mcopy, void*, dst, void*, src, size_t, len)
{
	char buf[64];

	if (len > 64)
		return -EINVAL;

	if (copy_from_user(buf, src, len))
		return -EFAULT;

	if (copy_to_user(dst, buf, len))
		return -EFAULT;

	return 0;
}


EXPORT_SYMBOL_GPL(probe_kernel_write);

/**
 * strncpy_from_unsafe: - Copy a NUL terminated string from unsafe address.
 * @dst:   Destination address, in kernel space.  This buffer must be at
 *         least @count bytes long.
 * @src:   Unsafe address.
 * @count: Maximum number of bytes to copy, including the trailing NUL.
 *
 * Copies a NUL-terminated string from unsafe address to kernel buffer.
 *
 * On success, returns the length of the string INCLUDING the trailing NUL.
 *
 * If access fails, returns -EFAULT (some data may have been copied
 * and the trailing NUL added).
 *
 * If @count is smaller than the length of the string, copies @count-1 bytes,
 * sets the last byte of @dst buffer to NUL and returns @count.
 */
long strncpy_from_unsafe(char *dst, const void *unsafe_addr, long count)
{
	mm_segment_t old_fs = get_fs();
	const void *src = unsafe_addr;
	long ret;

	if (unlikely(count <= 0))
		return 0;

	set_fs(KERNEL_DS);
	pagefault_disable();

	do {
		ret = __copy_from_user_inatomic(dst++,
						(const void __user __force *)src++, 1);
	} while (dst[-1] && ret == 0 && src - unsafe_addr < count);

	dst[-1] = '\0';
	pagefault_enable();
	set_fs(old_fs);

	return ret < 0 ? ret : src - unsafe_addr;
}
