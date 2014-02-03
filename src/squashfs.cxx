/**
 * SquashFS delta tools
 * (c) 2014 Michał Górny
 * Released under the terms of the 2-clause BSD license
 */

#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif

#include <cstring>

extern "C"
{
#ifdef HAVE_ENDIAN_H
#	include <endian.h>
#endif
}

#include "squashfs.hxx"

unsigned char* squashfs::dir_index::name()
{
	void* voidp = static_cast<void*>(this + 1);
	return static_cast<unsigned char*>(voidp);
}

char* squashfs::inode::symlink::symlink_name()
{
	void* voidp = static_cast<void*>(this + 1);
	return static_cast<char*>(voidp);
}

size_t squashfs::inode::symlink::inode_size()
{
	return sizeof(*this) + symlink_size;
}

le32* squashfs::inode::reg::block_list()
{
	void* voidp = static_cast<void*>(this + 1);
	return static_cast<le32*>(voidp);
}

uint32_t squashfs::inode::reg::block_count(uint32_t block_size,
		uint16_t block_log)
{
	uint32_t blocks = file_size;

	// if fragments were not used, round up the last block
	if (fragment == squashfs::invalid_frag)
		blocks += block_size - 1;

	// bytes -> blocks
	blocks >>= block_log;

	return blocks;
}

size_t squashfs::inode::reg::inode_size(uint32_t block_size,
		uint16_t block_log)
{
	uint32_t blocks = block_count(block_size, block_log);

	return sizeof(*this) + blocks * sizeof(le32);
}

le32* squashfs::inode::lreg::block_list()
{
	void* voidp = static_cast<void*>(this + 1);
	return static_cast<le32*>(voidp);
}

struct squashfs::dir_index* squashfs::inode::ldir::index()
{
	void* voidp = static_cast<void*>(this + 1);
	return static_cast<struct dir_index*>(voidp);
}

size_t squashfs::inode::lreg::inode_size(uint32_t block_size, uint16_t block_log)
{
	uint64_t blocks = file_size;

	// if fragments were not used, round up the last block
	if (fragment == squashfs::invalid_frag)
		blocks += block_size - 1;

	// bytes -> blocks
	blocks >>= block_log;

	return sizeof(*this) + 2 * blocks * sizeof(le16);
}

MetadataReader::MetadataReader(const MMAPFile& new_file,
		const struct squashfs::super_block& sb,
		const Compressor& c)
	: f(new_file), compressor(c),
	bufp(buf), buf_filled(0)
{
	f.seek(sb.inode_table_start, std::ios::beg);
}

void MetadataReader::poll_data()
{
	uint16_t length = f.read<le16>();
	char* writep = bufp + buf_filled;

	// if we're past half buffer, shift it
	if (writep - buf > squashfs::metadata_size)
	{
		// we're past half, so we won't have more than half :)
		// therefore, no risk of overlapping areas
		memcpy(buf, bufp, buf_filled);
		bufp = buf;
		writep = bufp + buf_filled;
	}

	if (length & squashfs::inode_size::uncompressed)
	{
		// uncompressed inode
		length &= ~squashfs::inode_size::uncompressed;
		memcpy(writep, f.read_array<char>(length), length);
		buf_filled += length;
	}
	else
	{
		// uncompress to buf
		// passing size of metadata_size since we don't expect a bigger
		// output and we can guarantee that we have at least that much free
		buf_filled += compressor.decompress(writep,
				f.read_array<char>(length), length, squashfs::metadata_size);
	}
}

void* MetadataReader::peek(size_t length)
{
	while (buf_filled < length)
		poll_data();

	return static_cast<void*>(bufp);
}

void MetadataReader::seek(size_t length)
{
	bufp += length;
	buf_filled -= length;
}

InodeReader::InodeReader(const MMAPFile& new_file,
		const struct squashfs::super_block& sb,
		const Compressor& c)
	: f(new_file, sb, c),
	inode_num(0), no_inodes(sb.inodes),
	block_size(sb.block_size), block_log(sb.block_log)
{
}

union squashfs::inode::inode& InodeReader::read()
{
	if (inode_num >= no_inodes+1)
		throw std::runtime_error("Trying to read past last inode");

	// start with inode 'header' size
	void* ret = f.peek(sizeof(squashfs::inode::base));
	struct squashfs::inode::base* in = static_cast<squashfs::inode::base*>(ret);

	// get the actual type-specific inode size
	size_t inode_len;
	switch (in->inode_type)
	{
		case squashfs::inode::type::dir:
			inode_len = sizeof(struct squashfs::inode::dir);
			break;
		case squashfs::inode::type::reg:
			inode_len = sizeof(struct squashfs::inode::reg);
			break;
		case squashfs::inode::type::symlink:
		case squashfs::inode::type::lsymlink:
			inode_len = sizeof(struct squashfs::inode::symlink);
			break;
		case squashfs::inode::type::blkdev:
		case squashfs::inode::type::chrdev:
			inode_len = sizeof(struct squashfs::inode::dev);
			break;
		case squashfs::inode::type::fifo:
		case squashfs::inode::type::socket:
			inode_len = sizeof(struct squashfs::inode::ipc);
			break;
		case squashfs::inode::type::ldir:
			inode_len = sizeof(struct squashfs::inode::ldir);
			break;
		case squashfs::inode::type::lreg:
			inode_len = sizeof(struct squashfs::inode::lreg);
			break;
		case squashfs::inode::type::lblkdev:
		case squashfs::inode::type::lchrdev:
			inode_len = sizeof(struct squashfs::inode::ldev);
			break;
		case squashfs::inode::type::lfifo:
		case squashfs::inode::type::lsocket:
			inode_len = sizeof(struct squashfs::inode::lipc);
			break;
	}
	if (!in->inode_type || in->inode_type > squashfs::inode::type::lsocket)
		throw std::runtime_error("Invalid inode type");

	ret = f.peek(inode_len);
	in = static_cast<squashfs::inode::base*>(ret);

	// now consider the inodes with dynamic sizes
	switch (in->inode_type)
	{
		case squashfs::inode::type::reg:
			{
				struct squashfs::inode::reg* in2 =
					static_cast<struct squashfs::inode::reg*>(ret);
				inode_len = in2->inode_size(block_size, block_log);
				break;
			}
		case squashfs::inode::type::symlink:
		case squashfs::inode::type::lsymlink:
			{
				struct squashfs::inode::symlink* in2 =
					static_cast<struct squashfs::inode::symlink*>(ret);
				inode_len = in2->inode_size();
				break;
			}
		case squashfs::inode::type::lreg:
			{
				struct squashfs::inode::lreg* in2 =
					static_cast<struct squashfs::inode::lreg*>(ret);
				inode_len = in2->inode_size(block_size, block_log);
				break;
			}
		case squashfs::inode::type::ldir:
			{
				struct squashfs::inode::ldir* in2 =
					static_cast<struct squashfs::inode::ldir*>(ret);

				// the header is followed by i_count dir indexes...
				// and each of those indexes is of variable size anyway.
				// but they will be at least sizeof(dir_index) long,
				// so we can at least start by reading that much.
				inode_len += in2->i_count * sizeof(struct squashfs::dir_index);
				break;
			}
	}

	ret = f.peek(inode_len);
	in = static_cast<squashfs::inode::base*>(ret);

	if (in->inode_type == squashfs::inode::type::ldir)
	{
		// continue scanning dir indexes
		struct squashfs::inode::ldir* in2 =
			static_cast<struct squashfs::inode::ldir*>(ret);

		size_t offset = sizeof(struct squashfs::inode::ldir);
		for (int i = 0; i < in2->i_count; ++i)
		{
			char* charp = static_cast<char*>(ret) + offset;
			void* voidp = static_cast<void*>(charp);
			struct squashfs::dir_index* idx
				= static_cast<struct squashfs::dir_index*>(voidp);

			// size is length-1... for some smart reason
			inode_len += idx->size + 1;
			offset += idx->size + 1 + sizeof(struct squashfs::dir_index);

			ret = f.peek(inode_len);
			in2 = static_cast<struct squashfs::inode::ldir*>(ret);
		}
	}

	// seek towards the next inode
	f.seek(inode_len);
	++inode_num;

	return *static_cast<union squashfs::inode::inode*>(ret);
}
