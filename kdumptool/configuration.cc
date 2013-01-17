/*
 * (c) 2008, Bernhard Walle <bwalle@suse.de>, SUSE LINUX Products GmbH
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
#include <string>

#include "configuration.h"
#include "configparser.h"

using std::string;

//{{{ Configuration ------------------------------------------------------------

const struct Configuration::OptionDesc Configuration::m_optiondesc[] = {
#define DEFINE_OPT(name, type, val, defval)		\
    { name, Configuration::OptionDesc::type_ ## type,	\
	{ .val_ ## type = &Configuration::val } },
#include "define_opt.h"
#undef DEFINE_OPT
};

Configuration *Configuration::m_instance = NULL;

// -----------------------------------------------------------------------------
Configuration *Configuration::config()
    throw ()
{
    if (!m_instance)
        m_instance = new Configuration();
    return m_instance;
}

// -----------------------------------------------------------------------------
Configuration::Configuration()
    throw ()
    : m_readConfig(false)
#define DEFINE_OPT(name, type, val, defval)		\
      , val(defval)
#include "define_opt.h"
#undef DEFINE_OPT
{}

/* -------------------------------------------------------------------------- */
void Configuration::readFile(const string &filename)
    throw (KError)
{
    unsigned i;
    ConfigParser cp(filename);

    for (i = 0; i < sizeof(m_optiondesc) / sizeof(m_optiondesc[0]); ++i)
	cp.addVariable(m_optiondesc[i].name);

    cp.parse();

    for (i = 0; i < sizeof(m_optiondesc) / sizeof(m_optiondesc[0]); ++i) {
	const struct OptionDesc *opt = &m_optiondesc[i];
	switch(opt->type) {
	case OptionDesc::type_string:
	    *this.*opt->val_string = cp.getValue(opt->name);
	    break;
	case OptionDesc::type_int:
	    *this.*opt->val_int = cp.getIntValue(opt->name);
	    break;
	case OptionDesc::type_bool:
	    *this.*opt->val_bool = cp.getBoolValue(opt->name);
	    break;
	}
    }

    m_readConfig = true;
}

// -----------------------------------------------------------------------------
string Configuration::getKernelVersion() const
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_kernelVersion;
}

// -----------------------------------------------------------------------------
int Configuration::getCPUs() const
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_CPUs;
}


// -----------------------------------------------------------------------------
string Configuration::getCommandLine() const
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_commandLine;
}


// -----------------------------------------------------------------------------
string Configuration::getCommandLineAppend() const
     throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_commandLineAppend;
}


// -----------------------------------------------------------------------------
std::string Configuration::getKexecOptions() const
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_kexecOptions;
}

// -----------------------------------------------------------------------------
string Configuration::getMakedumpfileOptions() const
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_makedumpfileOptions;
}

// -----------------------------------------------------------------------------
bool Configuration::getImmediateReboot() const
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_immediateReboot;
}


// -----------------------------------------------------------------------------
string Configuration::getCustomTransfer() const
     throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_customTransfer;
}


// -----------------------------------------------------------------------------
std::string Configuration::getSavedir() const
     throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_savedir;
}


// -----------------------------------------------------------------------------
int Configuration::getKeepOldDumps() const
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_keepOldDumps;
}


// -----------------------------------------------------------------------------
int Configuration::getFreeDiskSize() const
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_freeDiskSize;
}


// -----------------------------------------------------------------------------
int Configuration::getVerbose() const
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_verbose;
}


// -----------------------------------------------------------------------------
int Configuration::getDumpLevel() const
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_dumpLevel;
}


// -----------------------------------------------------------------------------
std::string Configuration::getDumpFormat() const
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_dumpFormat;
}


// -----------------------------------------------------------------------------
bool Configuration::getContinueOnError() const
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_continueOnError;
}


// -----------------------------------------------------------------------------
std::string Configuration::getRequiredPrograms() const
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_requiredPrograms;
}


// -----------------------------------------------------------------------------
std::string Configuration::getPrescript() const
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_prescript;
}


// -----------------------------------------------------------------------------
std::string Configuration::getPostscript() const
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_postscript;
}


// -----------------------------------------------------------------------------
bool Configuration::getCopyKernel() const
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_copyKernel;
}

// -----------------------------------------------------------------------------
std::string Configuration::getKdumptoolFlags() const
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_kdumptoolFlags;
}

// -----------------------------------------------------------------------------
bool Configuration::kdumptoolContainsFlag(const std::string &flag)
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    string value = getKdumptoolFlags();
    string::size_type pos = value.find(flag);
    return pos != string::npos;
}

// -----------------------------------------------------------------------------
string Configuration::getSmtpServer()
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_smtpServer;
}

// -----------------------------------------------------------------------------
string Configuration::getSmtpUser()
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_smtpUser;
}

// -----------------------------------------------------------------------------
string Configuration::getSmtpPassword()
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_smtpPassword;
}

// -----------------------------------------------------------------------------
string Configuration::getNotificationTo()
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_notificationTo;
}

// -----------------------------------------------------------------------------
string Configuration::getNotificationCc()
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_notificationCc;
}

// -----------------------------------------------------------------------------
string Configuration::getHostKey()
    throw (KError)
{
    if (!m_readConfig)
        throw KError("Configuration has not been read.");

    return m_hostKey;
}

//}}}

// vim: set sw=4 ts=4 fdm=marker et: :collapseFolds=1:
