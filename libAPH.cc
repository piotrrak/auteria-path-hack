/* vim: set sw=4 sts=4 et foldmethod=syntax : */

/*
 * Copyright (C) 2008 Piotr Rak <piotr.rak@gmail.com>
 *
 * Partly Copyright (C) 2001 Geert Bevin, Uwyn, http://www.uwyn.com
 * Partly Copyright (C) 2002  Martin Schlemmer <azarah@gentoo.org> 
 * Partly Copyright (C) 1998-9 Pancrazio `Ezio' de Mauro <p@demauro.net>,
 * as some of Sandbox (http://www.gentoo.org/) code was used
 *  
 *
 * This file is part of the libAPH. libAPH is free software;
 * you can redistribute it and/or modify it under the terms of the GNU General
 * Public License version 2, as published by the Free Software Foundation.
 *
 * libAPH distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#define open xxx_open

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>

#undef open

#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <stdexcept>
#include <pcre.h>

#define LIBC_VERSION "libc-2.6.1.so" ///< libc version

#define symname_open "open" ///< libc open() symbol name
#define symver_open NULL ///< libc open() symbol version

#define LIBAPH_NAME "libAPH: " ///< prefix string for lib output


/**
 * Library environment variable.
 * If set location where to store files, may be used to override $HOME/.auteria.
 */
#define LIBAPH_CONFIG_LOCATION_ENVVAR "LIBAPH_CONFIG_LOCATION"

/**
 * Library environment variable.
 * If set to "1", library will give additional runtime info
 */
#define LIBAPH_VERBOSE_ENVVAR "LIBAPH_VERBOSE"

/**
 * Handful macro for lazy loading symbols from libc
 */
#define check_sym(_name) \
do { \
    int old_errno = errno; \
    if(NULL == true_ ## _name) \
        true_ ## _name = reinterpret_cast<typeof(true_ ## _name)>(get_dlsym(symname_ ##_name, symver_ ## _name)); \
    errno = old_errno; \
} while(false);

/**
 * Returns ostream for verbose messages.
 */
static std::ostream&
verbose_out() __attribute__((warn_unused_result));

/**
 * Simple posix positional syntax string formatting.
 */
static std::ostream&
format(std::ostream&, const std::string&, const std::vector<std::string>&);

/**
 * Returns environment variable or default value if unset.
 */
static std::string
getenv_with_default(const std::string&, const std::string&);

/**
 * Adds slash at the end of string.
 */
static void
add_trailing_slash(std::string&);

/**
 * Returns configuration files store location.
 */
static std::string
get_config_files_location();

/**
 * Load function symbol from libc.
 */
static
void* get_dlsym(const char* name, const char *version);

/**
 * Tha Queen, open() wrapper.
 */
extern "C" __attribute__ ((visibility("default"))) int open(char * pathname, int flags, ...);

/**
 * NonCopyable object idiom.
 */
template <typename Ty_>
class NonCopyable
{
protected:
    NonCopyable() {}
private:
    NonCopyable(const Ty_&); //= delete
    const NonCopyable& operator= (const Ty_&); //= delete
};

/**
 * Represents path substition.
 */
struct Substition :
    NonCopyable<Substition>
{
    /**
     * Constructor.
     */
    Substition(const char*, const char*);

    /**
     * Destructor.
     */
    ~Substition();

    /**
     * Returns weather match for given path occured.
     */
    bool matches(const std::string&) const;

    /**
     * Deos substiution of recently matched path.
     * If none matched yet, returns NULL.
     */
    char* substitute() __attribute__((warn_unused_result));

    /**
     * Clears last match.
     */
    void clear_match() const;

private:
    /**
     *Returns weather regular exression was compiled already.
     */
    bool is_compiled() const;

    /**
     * Compiles regular expression.
     */
    void compile() const;

    /* Note: All mutable fields of this object should be protected from races,
     *       if open() might be called from different threads,
     *       altough research doesn't show it is issue ATM...
     */
    const char* _re_str; ///< Regular expression string
    const std::string _format; ///< Format string use for substiution (uses posix positional syntax) 
    mutable pcre* _re; ///< Compiled regular exression
    mutable pcre_extra* _pe; ///< Addition information for compiled regexp
    mutable int _substr_count; ///< Substring count in match
    mutable std::string _last_match; ///< Last matched path
    mutable int* _outvec; ///< Submatches indecies vector
    
#if 0
    mutex_t mutex; //protecting all mutable fields probably might be sane...
#endif
};


/**
 * NoOpOutputStream buffer implementation.
 */
class NoOpOutputStreamBuf :
    public std::streambuf
{
protected:

    virtual int_type
    overflow(int_type c)
    {
        // always success write single not EOF char
        return c;
    }

    virtual std::streamsize
    xsputn(char *, std::streamsize s)
    {
        //do nothing, always success write s chars
        return s;
    }
};

/**
 * Member from base initialization for NoOpOutputStream.
 */
class NoOpOutputStreamBase
{
protected:
    NoOpOutputStreamBuf buf;
};

/**
 * Output stream that does nothing (kind of /dev/null output).
 */
class NoOpOutputStream :
    protected NoOpOutputStreamBase,
    public std::ostream
{
public:
    /**
     * Constructor.
     */
    NoOpOutputStream() :
        NoOpOutputStreamBase(),
        std::ostream(&buf)
    {
    }
};

/*
 * To get explanation of NoOpOutputStream please refer to chapter 13.13.3 of "The C++ Standard Library"
 * Nicolai M. Josuttis / Addison-Wesley / ISBN 0-201-37926-0
 */

// GLOBALS

static void* libc_handle = NULL; ///< handle to dynloaded libc.so
static int (*true_open) (char *, int flags, ...) = NULL; ///< pointer to true open() from libc

/**
 * All path substiutions.
 */
static Substition SUBSTITUTE_PATHS[] = {
    Substition("game/client/(config\\.cs.*)$", "%1%"), //config.cs
    Substition("game/client/(prefs\\.cs.*)$", "%1%"), //prefs.cs
    Substition("console.log", "console.log"), //console.log
    Substition("log/([^/]+)/(.+\\.qlog)", "%1%@%2%"), // player/Map.qlog ->  player@Map.qlog
    Substition("log/([^/]+)/(questlog\\.cs.*)", "%1%@%2%"), // player/qestlog.cs -> player@questlog.cs
    Substition("log/([^/]+)/chat.log", "%1%@chat.log"), // player/chat.log -> player@chat.log
    Substition("(screenshot_.+)", "%1%")
};

// IMPLEMENATION

static std::ostream&
format(std::ostream& os, const std::string& frmt, const std::vector<std::string>& args_array)
{
    std::vector<std::string>::size_type args_count = args_array.size();
    std::string::size_type i(0), eaten(0);

    for(; std::string::npos != (i = frmt.find("%", i)); ++i)
    {
        if (frmt.size() == i+1) //we have found not matching '%' at end
            throw std::logic_error(
                std::string("Invalid format string `") + frmt + "'");

        if ('%'==frmt[++i]) //using escape "%%"
        {
            os << frmt.substr(eaten, i-1-eaten);
            eaten = i;
            continue;
        }

        if (eaten != i)
        {
            os << frmt.substr(eaten, i-1-eaten);
            eaten = i-1;
        }

        std::size_t arg_num(0);
        for (; frmt.size()>i ; ++i) //parse arg number
            if (! std::isdigit(frmt[i]))
                break;
            else
            {
                arg_num *= 10;
                arg_num += frmt[i] - '0';
            }

        if (0 >= arg_num || args_count < arg_num) // arg out of range
            throw std::logic_error(
                std::string("Format argument number out of range"));

        if ('%' != frmt[i]) // not %[0..9]*%
            throw std::logic_error(
                std::string("Invalid format string `") + frmt + "'");

        os << args_array[arg_num-1];

        eaten = i+1;
    }

    if (eaten < frmt.size())
        os << frmt.substr(eaten);
    return os;
}

static std::string
getenv_with_default(const std::string& env_var, const std::string& value)
{
    char * s(getenv(env_var.c_str()));
    if (! s)
        return value;

    std::string result(s);
    if (result.empty())
        return value;
    return result;
}

static void
add_trailing_slash(std::string& s)
{
    if (s.size() > 0 &&
        s[s.size()-1] != '/')
        s += "/";
}

static std::string
get_config_files_location()
{
    std::string location = getenv_with_default(LIBAPH_CONFIG_LOCATION_ENVVAR, "");
    if ( location.empty())
    {
        location = getenv_with_default("HOME", "");
        if (location.empty())
        {
            std::cerr << LIBAPH_NAME "Unable to get config files location.\n" 
                         LIBAPH_NAME "REASON: $HOME environment variable is not set!" << std::endl;
            exit(EXIT_FAILURE);
        }
        
        location += "/.auteria";
    }
    //XXX: canonicalize_file_name() glibc extension, realpath is br00ken tho...
    std::string canon_loc = canonicalize_file_name(location.c_str());
    add_trailing_slash(canon_loc);

    verbose_out() << LIBAPH_NAME "Will use location: `" << canon_loc << "'" << std::endl;
    
    return canon_loc;
}

static void*
get_dlsym(const char* name, const char *version)
{
    void * sym_addr = NULL;

    if (NULL == libc_handle)
    {
        libc_handle = dlopen(LIBC_VERSION, RTLD_LAZY);

        if (! libc_handle)
        {
            std::cerr << LIBAPH_NAME "Unable load libc!\n"
                      << LIBAPH_NAME "\t" << dlerror() << std::endl;
            exit(EXIT_FAILURE);
        }
    }

    if (NULL == version)
        sym_addr = dlsym(libc_handle, name);
    else
        sym_addr = reinterpret_cast<void* >(dlvsym(libc_handle, name, version));

    if (! sym_addr)
    {
        std::cerr << LIBAPH_NAME "Can't resolve symbol: `" << name << "'\n"
                  << LIBAPH_NAME "\t" << dlerror() << std::endl;
        exit(EXIT_FAILURE);
    }
        
    return sym_addr;
}

Substition
    ::Substition(const char* re, const char* f) :
    _re_str(re),
    _format(f),
    _re(),
    _pe(),
    _substr_count(),
    _last_match(),
    _outvec()
{
}

Substition
    ::~Substition()
{
    if (NULL != _re)
        (*pcre_free)(_re);
    if (NULL != _pe)
        (*pcre_free)(_pe);
    delete[] _outvec;
    _outvec = 0;
}

//XXX: not thread safe... 
bool
Substition
    ::matches(const std::string& pathname) const
{
    if (! is_compiled())
        compile();

    clear_match();
    //XXX: delete & new of _outvec seq 
    _outvec = new int[3*_substr_count];

    int match = pcre_exec(
        /*regexp*/      _re,
        /*study*/       _pe,
        /*subject*/     pathname.c_str(),
        /*subject len*/ pathname.size(),
        /*offset*/      0,
        /*options*/     0,
        /*matches*/     _outvec,
        /*matches len*/ 3*_substr_count
        );

    if (PCRE_ERROR_NOMATCH == match)
        return false;

    if (0 > match)
    {
        std::cerr << LIBAPH_NAME "Error matching `" << pathname << "'"
                  << " with regexp `" << _re_str << "'" <<std::endl;
        exit(EXIT_FAILURE);
    }

    _last_match = pathname;
    
    return true;
}

//XXX: again not thread safe... 
char*
Substition
    ::substitute()
{
    static std::string location = get_config_files_location();
    char * new_path = 0;
    std::vector<std::string> strings;

    std::ostringstream os;

    if (_last_match.empty())
    {
        std::cerr << LIBAPH_NAME "No match, huh!?" << std::endl;
        return NULL;
    }

    if (_substr_count > 1)
    {
        const char** substr_list;
        strings.reserve(_substr_count);
        pcre_get_substring_list(_last_match.c_str(), _outvec, _substr_count, &substr_list);
        for (int i=1; i < _substr_count; ++i) // substr_list[0] is whole match
        {
            strings.push_back(std::string(substr_list[i]));
        }
        pcre_free_substring_list(substr_list);
    }

    try
    {
        ::format(os, this->_format, strings);
        verbose_out() << LIBAPH_NAME "s/`" << _last_match << "'/`" << os.str() << "'/" 
                      << std::endl;
        std::string p = location + os.str();
        new_path = new char[p.size()+1];
        strcpy(new_path, p.c_str());
    }
    catch (std::exception& e)
    {
        std::cerr << LIBAPH_NAME "Error formatting `" << _format << "'\n"
                  << LIBAPH_NAME "\t" << e.what() << std::endl;
        exit(EXIT_FAILURE);
    }

    return new_path;
}

//XXX: and again not thread safe... 
void
Substition
    ::clear_match() const
{
    _last_match = "";
    delete[] _outvec;
    _outvec = 0;
}

//XXX: not thread safe too... 
bool
Substition
    ::is_compiled() const
{
    return NULL != _re;
}

//XXX: not thread safe too... 
void
Substition
    ::compile() const
{
    const char* error;
    int error_offset;
    int substr_count;
    
    pcre* re = pcre_compile(this->_re_str, 0, &error, &error_offset, NULL);
    if (NULL == re)
    {
        std::cerr << LIBAPH_NAME "Failed pcre_compile() with regexp `" << _re_str << "'\n"
                  << LIBAPH_NAME "\t" << error << std::endl;

        exit(EXIT_FAILURE);
    }
    
    error = NULL;
    pcre_extra* pe = pcre_study(re, 0, &error);
    if (NULL != error)
    {
        std::cerr << LIBAPH_NAME "Failed pcre_study() with regexp `" << _re_str << "'\n"
                  << LIBAPH_NAME "\t" << error << std::endl;

        exit(EXIT_FAILURE);
    }

    pcre_fullinfo(re, pe, PCRE_INFO_CAPTURECOUNT, &substr_count);
    ++substr_count;

    this->_re = re;
    this->_pe = pe;
    this->_substr_count = substr_count;
}

inline static char*
substitute_path_if_needed(std::string pathname)
{

    static const unsigned SUBST_PATHS_LEN = sizeof(SUBSTITUTE_PATHS) / sizeof(Substition);

    for (unsigned i=0; i < SUBST_PATHS_LEN; ++i)
    {
        Substition* s = SUBSTITUTE_PATHS + i;
        if (s->matches(pathname))
        {
            return s->substitute();
        }
    }
    return NULL;
}

static std::ostream&
verbose_out()
{
    static NoOpOutputStream noop_stream;
    static const bool verbose_output("1" == getenv_with_default(LIBAPH_VERBOSE_ENVVAR, "0"));

    if (verbose_output)
        return std::cout;
    else
        return noop_stream;
}

extern "C" int 
open(char * pathname, int flags, ...)
{
    va_list ap;
    int mode = 0;
    int result = -1;
    char* new_pathname;

    if (flags & O_CREAT)
    {
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }

    verbose_out() << LIBAPH_NAME "open(`" << pathname <<"', ...)" << std::endl;
    try
    {
        new_pathname = substitute_path_if_needed(pathname);
    }
    catch (std::exception& e) // memory allocation failure might be
    {
        std::cerr << LIBAPH_NAME "Unhandled exception: "
                  << LIBAPH_NAME "\t" << e.what() << std::endl;
        exit(EXIT_FAILURE);
    }
    catch (...) // just in case
    {
        std::cerr << LIBAPH_NAME "Unknown unhandled exception" << std::endl;
        exit(EXIT_FAILURE);
    }

    if (NULL != new_pathname)
        pathname = new_pathname;

    check_sym(open);

    if (flags & O_CREAT)
        result = true_open(pathname, flags, mode);
    else
        result = true_open(pathname, flags);

    delete[] new_pathname;

    return result;
}

