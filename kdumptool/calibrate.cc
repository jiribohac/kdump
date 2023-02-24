/*
 * (c) 2014, Petr Tesarik <ptesarik@suse.de>, SUSE LINUX Products GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */
#include <iostream>
#include <fstream>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <limits>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "subcommand.h"
#include "debug.h"
#include "calibrate.h"
#include "configuration.h"
#include "util.h"
#include "fileutil.h"
#include "mounts.h"
#include "process.h"
#include "rootdirurl.h"
#include "stringvector.h"
#include "configparser.h"

// All calculations are in KiB

// This macro converts MiB to KiB
#define MB(x)	((x)*1024)

// Shift right by an amount, rounding the result up
#define shr_round_up(n,amt)	(((n) + (1UL << (amt)) - 1) >> (amt))

// The following macros are defined:
//
//    DEF_RESERVE_KB	default reservation size
//    CAN_REDUCE_CPUS   non-zero if the architecture can reduce kernel
//                      memory requirements with nr_cpus=
//

#if defined(__x86_64__)
# define DEF_RESERVE_KB		MB(192)
# define CAN_REDUCE_CPUS	1

#elif defined(__i386__)
# define DEF_RESERVE_KB		MB(192)
# define CAN_REDUCE_CPUS	1

#elif defined(__powerpc64__)
# define DEF_RESERVE_KB		MB(384)
# define CAN_REDUCE_CPUS	0

#elif defined(__powerpc__)
# define DEF_RESERVE_KB		MB(192)
# define CAN_REDUCE_CPUS	0

#elif defined(__s390x__)
# define DEF_RESERVE_KB		MB(192)
# define CAN_REDUCE_CPUS	1

# define align_memmap		s390x_align_memmap

#elif defined(__s390__)
# define DEF_RESERVE_KB		MB(192)
# define CAN_REDUCE_CPUS	1

# define align_memmap		s390_align_memmap

#elif defined(__ia64__)
# define DEF_RESERVE_KB		MB(768)
# define CAN_REDUCE_CPUS	1

#elif defined(__aarch64__)
# define DEF_RESERVE_KB		MB(192)
# define CAN_REDUCE_CPUS	1

#elif defined(__arm__)
# define DEF_RESERVE_KB		MB(192)
# define CAN_REDUCE_CPUS	1

#else
# error "No default crashkernel reservation for your architecture!"
#endif

#ifndef align_memmap
# define align_memmap(maxpfn)	((unsigned long) (maxpfn))
#endif

static inline unsigned long s390_align_memmap(unsigned long maxpfn)
{
    // SECTION_SIZE_BITS: 25, PAGE_SHIFT: 12, KiB: 10
    return ((maxpfn - 1) | ((1UL << (25 - 12 - 10)) - 1)) + 1;
}

static inline unsigned long s390x_align_memmap(unsigned long maxpfn)
{
    // SECTION_SIZE_BITS: 28, PAGE_SHIFT: 12, KiB: 10
    return ((maxpfn - 1) | ((1UL << (28 - 12 - 10)) - 1)) + 1;
}

// Default framebuffer size: 1024x768 @ 32bpp, except on mainframe
#if defined(__s390__) || defined(__s390x__)
# define DEF_FRAMEBUFFER_KB	0
#else
# define DEF_FRAMEBUFFER_KB	(768UL*4)
#endif

// large hashes, default settings:			     -> per MiB
//   PID: sizeof(void*) for each 256 KiB			  4
//   Dentry cache: sizeof(void*) for each  8 KiB		128
//   Inode cache:  sizeof(void*) for each 16 KiB		 64
//   TCP established: 2*sizeof(void*) for each 128 KiB		 16
//   TCP bind: 2*sizeof(void*) for each 128 KiB			 16
//   UDP: 2*sizeof(void*) + 2*sizeof(long) for each 2 MiB	  1 + 1
//   UDP-Lite: 2*sizeof(void*) + 2*sizeof(long) for each 2 MiB	  1 + 1
//								-------
//								230 + 2
// Assuming that sizeof(void*) == sizeof(long):
#define KERNEL_HASH_PER_MB	(232*sizeof(long))

// Estimated buffer metadata and filesystem in KiB per dirty MiB
#define BUF_PER_DIRTY_MB	64

// Default vm dirty ratio is 20%
#define DIRTY_RATIO		20

// Reserve this much percent above the calculated value
#define ADD_RESERVE_PCT		30

// Reserve this much additional KiB above the calculated value
#define ADD_RESERVE_KB		MB(64)


// Maximum size of the page bitmap
// 32 MiB is 32*1024*1024*8 = 268435456 bits
// makedumpfile uses two bitmaps, so each has 134217728 bits
// with 4-KiB pages this covers 0.5 TiB of RAM in one cycle
#define MAX_BITMAP_KB	MB(32)

// Minimum lowmem allocation. This is 64M for swiotlb and 8M
// for overflow, DMA buffers, etc.
#define MINLOW_KB	MB(64 + 8)

using std::cerr;
using std::cout;
using std::endl;
using std::ifstream;
using std::string;

//{{{ SizeConstants ------------------------------------------------------------

class SizeConstants {
    protected:
        unsigned long m_kernel_base;
        unsigned long m_kernel_init;
        unsigned long m_kernel_init_net;
        unsigned long m_init_cached;
        unsigned long m_init_cached_net;
        unsigned long m_percpu;
        unsigned long m_pagesize;
        unsigned long m_sizeof_page;
        unsigned long m_user_base;
        unsigned long m_user_net;

    public:
        SizeConstants(void);

        /** Get kernel base requirements.
         *
         * This number covers all memory that can is unavailable to user
         * space, even if it is not allocated by the kernel itself, e.g.
         * if it is reserved by the firmware before the kernel starts.
         *
         * It also includes non-changing run-time allocations caused by
         * PID 1 initialisation (sysfs, procfs, etc.).
         *
         * @returns kernel base allocation [KiB]
         */
        unsigned long kernel_base_kb(void) const
        { return m_kernel_base; }

        /** Get additional kernel requirements at boot.
         *
         * This is memory which is required at boot time but gets freed
         * later. Most importantly, it includes the compressed initramfs
         * image.
         *
         * @returns boot-time requirements [KiB]
         */
        unsigned long kernel_init_kb(void) const
        { return m_kernel_init; }

        /** Get additional boot-time kernel requirements for network.
         *
         * @returns additional boot-time requirements for network [KiB]
         */
        unsigned long kernel_init_net_kb(void) const
        { return m_kernel_init_net; }

        /** Get size of the unpacked initramfs.
         *
         * This is the size of the base image, without network.
         *
         * @returns initramfs memory requirements [KiB]
         */
        unsigned long initramfs_kb(void) const
        { return m_init_cached; }

        /** Get the increase in unpacked intramfs size with network.
         *
         * @returns additional network initramfs memory requirements [KiB]
         */
        unsigned long initramfs_net_kb(void) const
        { return m_init_cached_net; }

        /** Get additional memory requirements per CPU.
         *
         * @returns kernel per-cpu allocation size [KiB]
         */
        unsigned long percpu_kb(void) const
        { return m_percpu; }

        /** Get target page size.
         *
         * @returns page size in BYTES
         */
        unsigned long pagesize(void) const
        { return m_pagesize; }

        /** Get the size of struct page.
         *
         * @returns sizeof(struct page) in BYTES
         */
        unsigned long sizeof_page(void) const
        { return m_sizeof_page; }

        /** Get base user-space requirements.
         *
         * @returns user-space base requirements [KiB]
         */
        unsigned long user_base_kb(void) const
        { return m_user_base; }

        /** Get the increas in user-space requirements with network.
         *
         * @returns additional network user-space requirements [KiB]
         */
        unsigned long user_net_kb(void) const
        { return m_user_net; }
};

// -----------------------------------------------------------------------------
SizeConstants::SizeConstants(void)
{
    static const struct {
        const char *const name;
        unsigned long SizeConstants::*const var;
    } vars[] = {
        { "KERNEL_BASE", &SizeConstants::m_kernel_base },
        { "KERNEL_INIT", &SizeConstants::m_kernel_init },
        { "INIT_NET", &SizeConstants::m_kernel_init_net },
        { "INIT_CACHED", &SizeConstants::m_init_cached },
        { "INIT_CACHED_NET", &SizeConstants::m_init_cached_net },
        { "PERCPU", &SizeConstants::m_percpu },
        { "PAGESIZE", &SizeConstants::m_pagesize },
        { "SIZEOFPAGE", &SizeConstants::m_sizeof_page },
        { "USER_BASE", &SizeConstants::m_user_base },
        { "USER_NET", &SizeConstants::m_user_net },
        { nullptr, nullptr }
    };
    ShellConfigParser cfg("/usr/lib/kdump/calibrate.conf");

    for (auto p = &vars[0]; p->name; ++p)
        cfg.addVariable(p->name, "");
    cfg.parse();
    for (auto p = &vars[0]; p->name; ++p) {
        KString val(cfg.getValue(p->name));
        if (val.empty())
            throw KError(std::string("No value configured for ") + p->name);
        this->*p->var = val.asLongLong();
    }
}

//}}}
//{{{ SystemCPU ----------------------------------------------------------------

// -----------------------------------------------------------------------------
unsigned long SystemCPU::count(const char *name)
{
    FilePath path(m_cpudir);
    path.appendPath(name);
    unsigned long ret = 0UL;

    ifstream fin(path.c_str());
    if (!fin)
	throw KSystemError("Cannot open " + path, errno);

    try {
	unsigned long n1, n2;
	char delim;

	fin.exceptions(ifstream::badbit);
	while (fin >> n1) {
	    fin >> delim;
	    if (fin && delim == '-') {
		if (! (fin >> n2) )
		    throw KError(path + ": wrong number format");
		ret += n2 - n1;
		fin >> delim;
	    }
	    if (!fin.eof() && delim != ',')
		throw KError(path + ": wrong delimiter: " + delim);
	    ++ret;
	}
	if (!fin.eof())
	    throw KError(path + ": wrong number format");
	fin.close();
    } catch (ifstream::failure &e) {
	throw KSystemError("Cannot read " + path, errno);
    }

    return ret;
}

//}}}
//{{{ HyperInfo ----------------------------------------------------------------

class HyperInfo {

    public:
        /**
         * Initialize a new HyperInfo object.
         *
         * @param[in] procdir Mount point for procfs
         * @param[in] sysdir  Mount point for sysfs
         */
        HyperInfo(const char *procdir = "/proc", const char *sysdir = "/sys");

    protected:
        std::string m_type, m_guest_type, m_guest_variant;

    private:
        /**
         * Read a file under a base directory into a string.
         */
        void read_str(std::string &str, const FilePath &basedir,
                      const char *attr);

    public:
        /**
         * Get hypervisor type.
         */
        const std::string& type(void) const
        { return m_type; }

        /**
         * Get hypervisor guest type.
         */
        const std::string& guest_type(void) const
        { return m_guest_type; }

        /**
         * Get hypervisor guest variant (Dom0 or DomU).
         */
        const std::string& guest_variant(void) const
        { return m_guest_variant; }
};

// -----------------------------------------------------------------------------
HyperInfo::HyperInfo(const char *procdir, const char *sysdir)
{
    FilePath basedir(sysdir);
    basedir.appendPath("hypervisor");

    read_str(m_type, basedir, "type");
    read_str(m_guest_type, basedir, "guest_type");

    if (m_type == "xen") {
        std::string caps;
        std::string::size_type pos, next, len;

        basedir = procdir;
        basedir.appendPath("xen");
        read_str(caps, basedir, "capabilities");

        m_guest_variant = "DomU";
        pos = 0;
        while (pos != std::string::npos) {
            len = next = caps.find(',', pos);
            if (next != std::string::npos) {
                ++next;
                len -= pos;
            }
            if (caps.compare(pos, len, "control_d") == 0) {
                m_guest_variant = "Dom0";
                break;
            }
            pos = next;
        }
    }
}

// -----------------------------------------------------------------------------
void HyperInfo::read_str(std::string &str, const FilePath &basedir,
                         const char *attr)
{
    FilePath fp(basedir);
    std::ifstream f;

    fp.appendPath(attr);
    f.open(fp.c_str());
    if (!f)
        return;

    getline(f, str);
    f.close();
    if (f.bad())
        throw KError(fp + ": Read failed");
}

//}}}
//{{{ Framebuffer --------------------------------------------------------------

class Framebuffer {

    public:
        /**
	 * Initialize a new Framebuffer object.
	 *
	 * @param[in] fbpath Framebuffer sysfs directory path
	 */
	Framebuffer(const char *fbpath)
	: m_dir(fbpath)
	{}

    protected:
        /**
	 * Path to the framebuffer device base directory
	 */
	const FilePath m_dir;

    public:
	/**
	 * Get length of the framebuffer [in bytes].
	 */
	unsigned long size(void) const;
};

// -----------------------------------------------------------------------------
unsigned long Framebuffer::size(void) const
{
    FilePath fp;
    std::ifstream f;
    unsigned long width, height, stride;
    char sep;

    fp.assign(m_dir);
    fp.appendPath("virtual_size");
    f.open(fp.c_str());
    if (!f)
	throw KError(fp + ": Open failed");
    f >> width >> sep >> height;
    f.close();
    if (f.bad())
	throw KError(fp + ": Read failed");
    else if (!f || sep != ',')
	throw KError(fp + ": Invalid content!");
    Debug::debug()->dbg("Framebuffer virtual size: %lux%lu", width, height);

    fp.assign(m_dir);
    fp.appendPath("stride");
    f.open(fp.c_str());
    if (!f)
	throw KError(fp + ": Open failed");
    f >> stride;
    f.close();
    if (f.bad())
	throw KError(fp + ": Read failed");
    else if (!f || sep != ',')
	throw KError(fp + ": Invalid content!");
    Debug::debug()->dbg("Framebuffer stride: %lu bytes", stride);

    return stride * height;
}

//}}}
//{{{ Framebuffers -------------------------------------------------------------

class Framebuffers {

    public:
        /**
	 * Initialize a new Framebuffer object.
	 *
	 * @param[in] sysdir Mount point for sysfs
	 */
	Framebuffers(const char *sysdir = "/sys")
	: m_fbdir(FilePath(sysdir).appendPath("class/graphics"))
	{}

    protected:
        /**
	 * Path to the base directory with links to framebuffer devices
	 */
	const FilePath m_fbdir;

        /**
	 * Filter only valid framebuffer device name
	 */
	class DirFilter : public ListDirFilter {

	    public:
		virtual ~DirFilter()
		{}

		bool test(int dirfd, const struct dirent *d) const
		{
		    char *end;
		    if (strncmp(d->d_name, "fb", 2))
			return false;
		    strtoul(d->d_name + 2, &end, 10);
		    return (*end == '\0' && end != d->d_name + 2);
		}
	};

    public:
	/**
	 * Get size of all framebuffers [in bytes].
	 */
	unsigned long size(void) const;
};

// -----------------------------------------------------------------------------
unsigned long Framebuffers::size(void) const
{
    Debug::debug()->trace("Framebuffers::size()");

    unsigned long ret = 0UL;

    StringVector v = m_fbdir.listDir(DirFilter());
    for (StringVector::const_iterator it = v.begin(); it != v.end(); ++it) {
        Debug::debug()->dbg("Found framebuffer: %s", it->c_str());

	FilePath fp(m_fbdir);
	fp.appendPath(*it);
	Framebuffer fb(fp.c_str());
	ret += fb.size();
    }

    Debug::debug()->dbg("Total size of all framebuffers: %lu bytes", ret);
    return ret;
}

//}}}
//{{{ SlabInfo -----------------------------------------------------------------

class SlabInfo {

    public:
        /**
	 * Initialize a new SlabInfo object.
	 *
	 * @param[in] line Line from /proc/slabinfo
	 */
	SlabInfo(const KString &line);

    protected:
	bool m_comment;
	KString m_name;
	unsigned long m_active_objs;
	unsigned long m_num_objs;
	unsigned long m_obj_size;
	unsigned long m_obj_per_slab;
	unsigned long m_pages_per_slab;
	unsigned long m_active_slabs;
	unsigned long m_num_slabs;

    public:
	bool isComment(void) const
	{ return m_comment; }

	const KString &name(void) const
	{ return m_name; }

	unsigned long activeObjs(void) const
	{ return m_active_objs; }

	unsigned long numObjs(void) const
	{ return m_num_objs; }

	unsigned long objSize(void) const
	{ return m_obj_size; }

	unsigned long objPerSlab(void) const
	{ return m_obj_per_slab; }

	unsigned long pagesPerSlab(void) const
	{ return m_pages_per_slab; }

	unsigned long activeSlabs(void) const
	{ return m_active_slabs; }

	unsigned long numSlabs(void) const
	{ return m_num_slabs; }
};

// -----------------------------------------------------------------------------
SlabInfo::SlabInfo(const KString &line)
{
    static const char slabdata_mark[] = " : slabdata ";

    std::istringstream ss(line);
    ss >> m_name;
    if (!ss)
	throw KError("Invalid slabinfo line: " + line);

    if (m_name[0] == '#') {
	m_comment = true;
	return;
    }
    m_comment = false;

    ss >> m_active_objs >> m_num_objs >> m_obj_size
       >> m_obj_per_slab >> m_pages_per_slab;
    if (!ss)
	throw KError("Invalid slabinfo line: " + line);

    size_t sdpos = line.find(slabdata_mark, ss.tellg());
    if (sdpos == KString::npos)
	throw KError("Invalid slabinfo line: " + line);

    ss.seekg(sdpos + sizeof(slabdata_mark) - 1, ss.beg);
    ss >> m_active_slabs >> m_num_slabs;
    if (!ss)
	throw KError("Invalid slabinfo line: " + line);
}

//}}}
//{{{ SlabInfos ----------------------------------------------------------------

// Taken from procps:
#define SLABINFO_LINE_LEN	2048
#define SLABINFO_VER_LEN	100

class SlabInfos {

    public:
	typedef std::map<KString, const SlabInfo*> Map;

        /**
	 * Initialize a new SlabInfos object.
	 *
	 * @param[in] procdir Mount point for procfs
	 */
	SlabInfos(const char *procdir = "/proc")
	: m_path(FilePath(procdir).appendPath("slabinfo"))
	{}

	~SlabInfos()
	{ destroyInfo(); }

    protected:
        /**
	 * Path to the slabinfo file
	 */
	const FilePath m_path;

        /**
	 * SlabInfo for each slab
	 */
	Map m_info;

    private:
	/**
	 * Destroy SlabInfo objects in m_info.
	 */
	void destroyInfo(void);

    public:
        /**
	 * Read the information about each slab.
	 */
	const Map& getInfo(void);
};

// -----------------------------------------------------------------------------
void SlabInfos::destroyInfo(void)
{
    Map::iterator it;
    for (it = m_info.begin(); it != m_info.end(); ++it)
	delete it->second;
    m_info.clear();
}

// -----------------------------------------------------------------------------
const SlabInfos::Map& SlabInfos::getInfo(void)
{
    static const char verhdr[] = "slabinfo - version: ";
    char buf[SLABINFO_VER_LEN], *p, *end;
    unsigned long major, minor;

    std::ifstream f(m_path.c_str());
    if (!f)
	throw KError(m_path + ": Open failed");
    f.getline(buf, sizeof buf);
    if (f.bad())
	throw KError(m_path + ": Read failed");
    else if (!f || strncmp(buf, verhdr, sizeof(verhdr)-1))
	throw KError(m_path + ": Invalid version");
    p = buf + sizeof(verhdr) - 1;

    major = strtoul(p, &end, 10);
    if (end == p || *end != '.')
	throw KError(m_path + ": Invalid version");
    p = end + 1;
    minor = strtoul(p, &end, 10);
    if (end == p || *end != '\0')
	throw KError(m_path + ": Invalid version");
    Debug::debug()->dbg("Found slabinfo version %lu.%lu", major, minor);

    if (major != 2)
	throw KError(m_path + ": Unsupported slabinfo version");

    char line[SLABINFO_LINE_LEN];
    while(f.getline(line, SLABINFO_LINE_LEN)) {
	SlabInfo *si = new SlabInfo(line);
	if (si->isComment()) {
	    delete si;
	    continue;
	}
	m_info[si->name()] = si;
    }

    return m_info;
}

//}}}
//{{{ MemRange -----------------------------------------------------------------

class MemRange {

    public:

        typedef unsigned long long Addr;

        /**
	 * Initialize a new MemRange object.
	 *
	 * @param[in] start First address within the range
	 * @param[in] end   Last address within the range
	 */
	MemRange(Addr start, Addr end)
	: m_start(start), m_end(end)
	{}

	/**
	 * Get first address in range.
	 */
	Addr start(void) const
	{ return m_start; }

	/**
	 * Get last address in range.
	 */
	Addr end(void) const
	{ return m_end; }

	/**
	 * Get range length.
	 *
	 * @return number of bytes in the range
	 */
	Addr length() const
	{ return m_end - m_start + 1; }

    protected:

	Addr m_start, m_end;
};

//}}}
//{{{ MemMap -------------------------------------------------------------------

class MemMap {

    public:

        typedef std::list<MemRange> List;

        /**
	 * Initialize a new MemMap object.
	 *
	 * @param[in] procdir Mount point for procfs
	 */
        MemMap(const SizeConstants &sizes, const char *procdir = "/proc");

	/**
	 * Get the total System RAM (in bytes).
	 */
	unsigned long long total(void) const;

	/**
	 * Get the size (in bytes) of the largest block up to
	 * a given limit.
	 *
	 * @param[in] limit  maximum address to be considered
	 */
	unsigned long long largest(unsigned long long limit) const;

	/**
	 * Get the size (in bytes) of the largest block.
	 */
	unsigned long long largest(void) const
	{ return largest(std::numeric_limits<unsigned long long>::max()); }

	/**
	 * Try to allocate a block.
	 */
	unsigned long long find(unsigned long size, unsigned long align) const;

    private:

        const SizeConstants& m_sizes;
	List m_ranges;
        MemRange::Addr m_kstart, m_kend;
};

MemMap::MemMap(const SizeConstants &sizes, const char *procdir)
    : m_sizes(sizes), m_kstart(0), m_kend(0)
{
    FilePath path(procdir);

    path.appendPath("iomem");
    ifstream f(path.c_str());
    if (!f)
	throw KError(path + ": Open failed");

    f.setf(std::ios::hex, std::ios::basefield);
    while (f) {
        int firstc = f.peek();
        if (!f.eof()) {
	    MemRange::Addr start, end;

	    if (!(f >> start))
		throw KError("Invalid resource start");
	    if (f.get() != '-')
		throw KError("Invalid range delimiter");
	    if (!(f >> end))
		throw KError("Invalid resource end");

	    int c;
	    while ((c = f.get()) == ' ');
	    if (c != ':')
		throw KError("Invalid resource name delimiter");
	    while ((c = f.get()) == ' ');
	    f.unget();

            KString name;
	    std::getline(f, name);
            if (firstc != ' ' && name == "System RAM") {
                m_ranges.emplace_back(start, end);
            } else if (firstc == ' ' && name.startsWith("Kernel ")) {
                if (!m_kstart)
                    m_kstart = start;
                m_kend = end;
            }
        }
    }

    f.close();
}

// -----------------------------------------------------------------------------
unsigned long long MemMap::total(void) const
{
    unsigned long long ret = 0;

    for (const auto& range : m_ranges)
        ret += range.length();

    return ret;
}

// -----------------------------------------------------------------------------
unsigned long long MemMap::largest(unsigned long long limit) const
{
    unsigned long long ret = 0;

    for (const auto& range : m_ranges) {
	MemRange::Addr start, end, length;

        start = range.start();
	if (start > limit)
	    continue;

        end = range.end();
        if (end > limit)
            end = limit;
        length = end - start + 1;
        if (start <= m_kstart && m_kstart <= end) {
            // Worst case is if the kernel and initrd are spread evenly
            // across this RAM region
            MemRange::Addr ksize =
                (end < m_kend ? end : m_kend) - m_kstart + 1;
            length = (length - ksize - m_sizes.kernel_init_kb()) / 3;
        }
	if (length > ret)
	    ret = length;
    }

    return ret;
}

// -----------------------------------------------------------------------------
unsigned long long MemMap::find(unsigned long size, unsigned long align) const
{
    List::const_reverse_iterator it;

    for (it = m_ranges.rbegin(); it != m_ranges.rend(); ++it) {
        MemRange::Addr base = it->end() + 1;

	if (base < size)
	    continue;

	base -= size;
	base -= base % align;
        if (base >= it->start())
	    return base;
    }

    return ~0ULL;
}

//}}}
//{{{ CryptInfo ----------------------------------------------------------------

/**
 * Given a LUKS crypto device, dump the header using 'cryptinfo' and
 * parse the output looking for maximum memory requirements.
 */
class CryptInfo {
        unsigned long m_memory;

    public:
        CryptInfo(std::string const& device);

	/**
	 * Get memory requirements.
	 *
	 * @return Maximum memory in KiB needed to open the device
	 */
        unsigned long memory(void) const
        { return m_memory; }
};

// -----------------------------------------------------------------------------
CryptInfo::CryptInfo(std::string const& device)
    : m_memory(0)
{
    Debug::debug()->trace("CryptInfo::CryptInfo(%s)", device.c_str());

    ProcessFilter p;

    StringVector args;
    args.push_back("luksDump");
    args.push_back(device);

    std::ostringstream stdoutStream, stderrStream;
    p.setStdout(&stdoutStream);
    p.setStderr(&stderrStream);
    int ret = p.execute("cryptsetup", args);
    if (ret != 0) {
        KString error = stderrStream.str();
        throw KError("cryptsetup failed: " + error.trim());
    }

    KString out = stdoutStream.str();
    size_t pos = 0;
    while (pos < out.length()) {
        size_t end = out.find_first_of("\r\n", pos);
        size_t sep = out.find(':', pos);
        if (sep < end) {
            KString key = out.substr(pos, sep - pos);
            if (key.trim() == "Memory") {
                KString val = out.substr(sep + 1, end - sep - 1);
                unsigned long memory = val.trim().asLongLong();
                Debug::debug()->dbg("Crypto device %s needs %lu KiB",
                                    device.c_str(), memory);
                if (memory > m_memory)
                    m_memory = memory;
            }
        }
        pos = out.find_first_not_of("\r\n", end);
    }
}

//}}}
//{{{ Calibrate ----------------------------------------------------------------

// -----------------------------------------------------------------------------
Calibrate::Calibrate()
    : m_shrink(false)
{
    Debug::debug()->trace("Calibrate::Calibrate()");

    m_options.push_back(new FlagOption("shrink", 's', &m_shrink,
        "Shrink the crash kernel reservation"));
}

// -----------------------------------------------------------------------------
const char *Calibrate::getName() const
{
    return "calibrate";
}

// -----------------------------------------------------------------------------
static unsigned long runtimeSize(SizeConstants const &sizes,
                                 unsigned long memtotal)
{
    Configuration *config = Configuration::config();
    unsigned long required, prev;

    // Run-time kernel requirements
    required = sizes.kernel_base_kb() + sizes.initramfs_kb();

    // Double the size, because fbcon allocates its own framebuffer,
    // and many DRM drivers allocate the hw framebuffer in system RAM
    try {
        Framebuffers fb;
        required += 2 * fb.size() / 1024UL;
    } catch(KError &e) {
        Debug::debug()->dbg("Cannot get framebuffer size: %s", e.what());
        required += 2 * DEF_FRAMEBUFFER_KB;
    }

    // LUKS Argon2 hash requires a lot of memory
    try {
        FilesystemTypeMap map;

        if (config->KDUMP_COPY_KERNEL.value()) {
            try {
                map.addPath("/boot");
            } catch (KError&) {
                // ignore device resolution failures
            }
        }

        std::istringstream iss(config->KDUMP_SAVEDIR.value());
        std::string elem;
        while (iss >> elem) {
            RootDirURL url(elem, std::string());
            if (url.getProtocol() == RootDirURL::PROT_FILE) {
                try {
                    map.addPath(url.getRealPath());
                } catch (KError&) {
                    // ignore device resolution failures
                }
            }
        }

        unsigned long crypto_mem = 0;
        for (const auto& devmap : map.devices()) {
            if (devmap.second == "crypto_LUKS") {
                CryptInfo info(devmap.first);
                if (crypto_mem < info.memory())
                    crypto_mem = info.memory();
            }
        }
        required += crypto_mem;

        Debug::debug()->dbg("Adding %lu KiB for crypto devices", crypto_mem);
    } catch (KError &e) {
        Debug::debug()->dbg("Cannot check encrypted volumes: %s", e.what());
        // Fall back to no allocation
    }

    // Add space for constant slabs
    try {
        SlabInfos slab;
        for (const auto& elem : slab.getInfo()) {
            if (elem.first.startsWith("Acpi-")) {
                unsigned long slabsize = elem.second->numSlabs() *
                    elem.second->pagesPerSlab() * sizes.pagesize() / 1024;
                required += slabsize;

                Debug::debug()->dbg("Adding %ld KiB for %s slab cache",
                                    slabsize, elem.second->name().c_str());
            }
        }
    } catch (KError &e) {
        Debug::debug()->dbg("Cannot get slab sizes: %s", e.what());
    }

    // Add memory based on CPU count
    unsigned long cpus = 0;
    if (CAN_REDUCE_CPUS)
        cpus = config->KDUMP_CPUS.value();
    if (!cpus) {
        SystemCPU syscpu;
        unsigned long online = syscpu.numOnline();
        unsigned long offline = syscpu.numOffline();
        Debug::debug()->dbg("CPUs online: %lu, offline: %lu",
                            online, offline);
        cpus = online + offline;
    }
    Debug::debug()->dbg("Total assumed CPUs: %lu", cpus);
    cpus *= sizes.percpu_kb();
    Debug::debug()->dbg("Total per-cpu requirements: %lu KiB", cpus);
    required += cpus;

    // User-space requirements
    unsigned long user = sizes.user_base_kb();
    if (config->needsNetwork())
        user += sizes.user_net_kb();

    if (config->needsMakedumpfile()) {
        // Estimate bitmap size (1 bit for every RAM page)
        unsigned long bitmapsz = shr_round_up(memtotal / sizes.pagesize(), 2);
        if (bitmapsz > MAX_BITMAP_KB)
            bitmapsz = MAX_BITMAP_KB;
        Debug::debug()->dbg("Estimated bitmap size: %lu KiB", bitmapsz);
        user += bitmapsz;

        // Makedumpfile needs additional 96 B for every 128 MiB of RAM
        user += 96 * shr_round_up(memtotal, 20 + 7);
    }
    Debug::debug()->dbg("Total userspace: %lu KiB", user);
    required += user;

    // Make room for dirty pages and in-flight I/O:
    //
    //   required = prev + dirty + io
    //      dirty = total * (DIRTY_RATIO / 100)
    //         io = dirty * (BUF_PER_DIRTY_MB / 1024)
    //
    // solve the above using integer math:
    unsigned long dirty;
    prev = required;
    required = required * MB(100) /
        (MB(100) - MB(DIRTY_RATIO) - DIRTY_RATIO * BUF_PER_DIRTY_MB);
    dirty = (required - prev) * MB(1) / (MB(1) + BUF_PER_DIRTY_MB);
    Debug::debug()->dbg("Dirty pagecache: %lu KiB", dirty);
    Debug::debug()->dbg("In-flight I/O: %lu KiB", required - prev - dirty);

    // Account for "large hashes"
    prev = required;
    required = required * MB(1024) / (MB(1024) - KERNEL_HASH_PER_MB);
    Debug::debug()->dbg("Large kernel hashes: %lu KiB", required - prev);

    // Add space for memmap
    prev = required;
    required = required * sizes.pagesize() / (sizes.pagesize() - sizes.sizeof_page());
    unsigned long maxpfn = (required - prev) / sizes.sizeof_page();
    required = prev + align_memmap(maxpfn) * sizes.sizeof_page();

    Debug::debug()->dbg("Maximum memmap size: %lu KiB", required - prev);
    Debug::debug()->dbg("Total run-time size: %lu KiB", required);
    return required;
}

// -----------------------------------------------------------------------------
static void shrink_crash_size(unsigned long size)
{
    static const char crash_size_fname[] = "/sys/kernel/kexec_crash_size";

    std::ostringstream ss;
    ss << size;
    const string &numstr = ss.str();

    int fd = open(crash_size_fname, O_WRONLY);
    if (fd < 0)
        throw KSystemError(string("Cannot open ") + crash_size_fname,
                           errno);
    if (write(fd, numstr.c_str(), numstr.length()) < 0) {
        if (errno == EINVAL)
            Debug::debug()->dbg("New crash kernel size is bigger");
        else if (errno == ENOENT)
            throw KError("Crash kernel is currently loaded");
        else
            throw KSystemError(string("Cannot write to ") + crash_size_fname,
                               errno);
    }
    close(fd);
}

// -----------------------------------------------------------------------------
void Calibrate::execute()
{
    Debug::debug()->trace("Calibrate::execute()");

    HyperInfo hyper;
    Debug::debug()->dbg("Hypervisor type: %s", hyper.type().c_str());
    Debug::debug()->dbg("Guest type: %s", hyper.guest_type().c_str());
    Debug::debug()->dbg("Guest variant: %s", hyper.guest_variant().c_str());
    if (hyper.type() == "xen" && hyper.guest_type() == "PV" &&
        hyper.guest_variant() == "DomU") {
        cout << "Total: 0" << endl;
        cout << "Low: 0" << endl;
        cout << "High: 0" << endl;
        cout << "MinLow: 0" << endl;
        cout << "MaxLow: 0" << endl;
        cout << "MinHigh: 0 " << endl;
        cout << "MaxHigh: 0 " << endl;
        return;
    }

    Configuration *config = Configuration::config();
    SizeConstants sizes;
    MemMap mm(sizes);
    unsigned long required;
    unsigned long memtotal = shr_round_up(mm.total(), 10);

    // Get total RAM size
    Debug::debug()->dbg("Expected total RAM: %lu KiB", memtotal);

    // Calculate boot requirements
    unsigned long bootsize = sizes.kernel_base_kb() +
        sizes.kernel_init_kb() + sizes.initramfs_kb();
    if (config->needsNetwork())
        bootsize += sizes.kernel_init_net_kb() + sizes.initramfs_net_kb();
    Debug::debug()->dbg("Memory needed at boot: %lu KiB", bootsize);

    try {
        required = runtimeSize(sizes, memtotal);

	// Make sure there is enough space at boot
	if (required < bootsize)
	    required = bootsize;

        // Reserve a fixed percentage on top of the calculation
        required = (required * (100 + ADD_RESERVE_PCT)) / 100 + ADD_RESERVE_KB;

    } catch(KError &e) {
	Debug::debug()->info(e.what());
	required = DEF_RESERVE_KB;
    }

    unsigned long low, minlow, maxlow;
    unsigned long high, minhigh, maxhigh;

#if defined(__x86_64__)

    unsigned long long base = mm.find(required << 10, 16UL << 20);

    Debug::debug()->dbg("Estimated crash area base: 0x%llx", base);

    // If maxpfn is above 4G, SWIOTLB may be needed
    if ((base + (required << 10)) >= (1ULL<<32)) {
	Debug::debug()->dbg("Adding 64 MiB for SWIOTLB");
	required += MB(64);
    }

    if (base < (1ULL<<32)) {
        low = minlow = 0;
    } else {
        low = minlow = MINLOW_KB;
        required = (required > low ? required - low : 0);
        if (required < bootsize)
            required = bootsize;
    }
    high = required;

    maxlow = mm.largest(1ULL<<32) >> 10;
    minhigh = 0;
    maxhigh = mm.largest() >> 10;

#else  // __x86_64__

    minlow = MINLOW_KB;
    low = required;

# if defined(__i386__)
    maxlow = mm.largest(512ULL<<20) >> 10;
# else
    maxlow = mm.largest() >> 10;
# endif  // __i386__

    high = 0;
    minhigh = 0;
    maxhigh = 0;

#endif  // __x86_64__

    cout << "Total: " << (memtotal >> 10) << endl;
    cout << "Low: " << shr_round_up(low, 10) << endl;
    cout << "High: " << shr_round_up(high, 10) << endl;
    cout << "MinLow: " << shr_round_up(minlow, 10) << endl;
    cout << "MaxLow: " << (maxlow >> 10) << endl;
    cout << "MinHigh: " << shr_round_up(minhigh, 10) << endl;
    cout << "MaxHigh: " << (maxhigh >> 10) << endl;

#if HAVE_FADUMP
    unsigned long fadump, minfadump, maxfadump;

    /* min = 64 MB, max = 50% of total memory, suggested = 5% of total memory */
    maxfadump = memtotal / 2;
    minfadump = MB(64);
    if (maxfadump < minfadump)
        maxfadump = minfadump;
    fadump = memtotal / 20;
    if (fadump < minfadump)
        fadump = minfadump;
    if (fadump > maxfadump)
        fadump = maxfadump;


    cout << "Fadump: "    << shr_round_up(fadump, 10) << endl;
    cout << "MinFadump: " << shr_round_up(minfadump, 10) << endl;
    cout << "MaxFadump: " << shr_round_up(maxfadump, 10) << endl;
#endif

    if (m_shrink)
        shrink_crash_size(required << 10);
}

//}}}

// vim: set sw=4 ts=4 fdm=marker et: :collapseFolds=1:
