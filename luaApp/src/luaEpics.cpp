#include <string>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <map>

#if defined(__vxworks) || defined(vxWorks)
	#include <symLib.h>
	#include <sysSymTbl.h>
#endif

#include <macLib.h>
#include <iocsh.h>
#include <epicsStdio.h>
#include <epicsFindSymbol.h>
#include <epicsExport.h>

#define epicsExportSharedSymbols
#include "luaEpics.h"


typedef std::vector<std::pair<const char*, lua_CFunction> >::iterator reg_iter;

static std::vector<std::pair<const char*, lua_CFunction> > registered_libs;
static std::vector<std::pair<const char*, lua_CFunction> > registered_funcs;

static std::map<std::string, lua_State*> named_states;

static FILE* temp_help = tmpfile();

/* Hook Routines */

epicsShareDef LUA_LIBRARY_LOAD_HOOK_ROUTINE luaLoadLibraryHook = NULL;
epicsShareDef LUA_FUNCTION_LOAD_HOOK_ROUTINE luaLoadFunctionHook = NULL;

epicsShareFunc std::string luaLocateFile(std::string filename)
{
	/* Check if the filename is an absolute path */
	if (filename.at(0) == '/' && std::ifstream(filename.c_str()).good())    { return filename; }

	/* Otherwise, see if the file exists in the script path */
	char* env_path = std::getenv("LUA_SCRIPT_PATH");

	#if defined(__vxworks) || defined(vxWorks)
	/* For compatibility reasons look for global symbols */
	if (!env_path)
	{
		char* symbol;
		SYM_TYPE type;

		if (symFindByName(sysSymTbl, "LUA_SCRIPT_PATH", &symbol, &type) == OK)
		{
			env_path = *(char**) symbol;
		}
	}
	#endif

	std::stringstream path;

	if   (env_path)
	{ 
		path << env_path;
		path << ":";
	}
	
	path << ".";
	
	std::string segment;

	while (std::getline(path, segment, ':'))
	{
		std::string fullpath = segment + "/" + filename;

		/* Check if file exists. If so, return the full filepath */
		if (std::ifstream(fullpath.c_str()).good())    { return fullpath; }
	}

	return "";
}


epicsShareFunc int luaLoadScript(lua_State* state, const char* script_file)
{
	std::string found = luaLocateFile(std::string(script_file));

	if (found.empty())    { return -1; }

	int status = luaL_loadfile(state, found.c_str());

	if (status)    { return status; }

	/* Run the script so that functions are in the global state */
	return lua_pcall(state, 0, 0, 0);

}

epicsShareFunc int luaLoadString(lua_State* state, const char* lua_code)
{
	if (std::string(lua_code).empty())    { return -1; }

	return luaL_loadstring(state, lua_code);
}

static void strtolua(lua_State* state, std::string text)
{
	size_t trim_front = text.find_first_not_of(" ");
	size_t trim_back  = text.find_last_not_of(" ");

	text = text.substr(trim_front, trim_back - trim_front + 1);

	std::stringstream convert;
	convert << "return " << text;
	
	lua_State* sandbox = luaL_newstate();
	luaL_dostring(sandbox, convert.str().c_str());
	
	int type = lua_type(sandbox, -1);
	
	if      (type == LUA_TNUMBER)  { lua_pushnumber(state, lua_tonumber(sandbox, -1)); }
	else if (type == LUA_TSTRING)  { lua_pushstring(state, lua_tostring(sandbox, -1)); }
	else if (type == LUA_TBOOLEAN) { lua_pushboolean(state, lua_toboolean(sandbox, -1)); }
	else                           { lua_pushstring(state, text.c_str()); }
}


epicsShareFunc int luaLoadParams(lua_State* state, const char* param_list)
{
	std::stringstream parse(param_list);
	std::string param;

	int num_params = 0;

	while (std::getline(parse, param, ','))
	{
		strtolua(state, param);
		num_params += 1;
	}

	return num_params;
}

epicsShareFunc void luaLoadMacros(lua_State* state, const char* macro_list)
{
	char** pairs;

	if (macro_list)
	{
		macParseDefns(NULL, macro_list, &pairs);

		for ( ; pairs && pairs[0]; pairs += 2)
		{
			std::string param(pairs[1]);

			strtolua(state, param);

			lua_setglobal(state, pairs[0]);
		}
	}
}

epicsShareFunc void luaRegisterLibrary(const char* library_name, lua_CFunction library_func)
{	
	std::pair<const char*, lua_CFunction> temp(library_name, library_func);

	registered_libs.push_back(temp);

	if (luaLoadLibraryHook)    { luaLoadLibraryHook(library_name, library_func); }
}

epicsShareFunc void luaRegisterFunction(const char* function_name, lua_CFunction function)
{
	std::pair<const char*, lua_CFunction> temp(function_name, function);

	registered_funcs.push_back(temp);

	if (luaLoadFunctionHook)    { luaLoadFunctionHook(function_name, function); }
}

static int luaCheckLibrary(lua_State* state)
{
	std::string libname(lua_tostring(state, 1));
	
	for (reg_iter index = registered_libs.begin(); index != registered_libs.end(); index++)
	{
		if (libname == std::string(index->first))
		{
			lua_pushcfunction(state, index->second);
			lua_pushnil(state);
			return 2;
		}
	}
	
	std::stringstream funcname;
	funcname << "\n\tno library registered '" << libname << "'";
	
	lua_pushstring(state, funcname.str().c_str());
	return 1;
}

epicsShareFunc void luaLoadRegistered(lua_State* state)
{
	/**
	 * Add luaCheckLibrary as an additional function for 
	 * lua to use to find libraries when using 'require'
	 */
	lua_getglobal(state, "package");
	lua_getfield(state, -1, "searchers");
	lua_len(state, -1);
	int num_searchers = lua_tonumber(state, -1);
	lua_pop(state, 1);
	lua_pushcfunction(state, luaCheckLibrary);
	lua_seti(state, -2, num_searchers + 1);
	lua_pop(state, 2);
	
	for (reg_iter index = registered_funcs.begin(); index != registered_funcs.end(); index++)
	{
		lua_register( state, index->first, index->second);
	}	
}


static int l_call(lua_State* state)
{
	lua_getfield(state, 1, "function_name");
	const char* function_name = lua_tostring(state, lua_gettop(state));
	lua_pop(state, 1);

	int given_args = lua_gettop(state) - 1;

    std::stringstream parameters;

	for(int index = 0; index < given_args; index += 1)
	{
		if (index > 0)    { parameters << ", "; }

		switch (lua_type(state, index + 2))
		{
			case LUA_TNIL:
				parameters << "0";
				break;

			case LUA_TNUMBER:
				parameters << lua_tostring(state, index + 2);
				break;

			case LUA_TSTRING:
				parameters << '"';
				parameters << lua_tostring(state, index + 2);
				parameters << '"';
				break;

			case LUA_TBOOLEAN:
				parameters << (lua_toboolean(state, index + 2) ? "1" : "0");
				break;

			case LUA_TLIGHTUSERDATA:
			{
				void* userdata = lua_touserdata(state, index + 2);

				lua_getglobal(state, "pdbbase");
				void* pdb = lua_touserdata(state, -1);
				lua_pop(state, 1);

				if (userdata == pdb)    { parameters << "pdbbase"; }
				else                    { parameters << "\"\""; }

				break;
			}

			default:
				parameters << "\"\"";
				break;
		}
	}

	std::stringstream cmd;
	cmd << function_name;
	cmd << "(";
	cmd << parameters.str();
	cmd << ")";

	iocshCmd(cmd.str().c_str());

	return 0;
}

static bool parseHelp(const char* func_name)
{
	// If there was an issue with initial tmpfile, attempt retry
	if (temp_help == NULL)
	{
		temp_help = tmpfile();
		
		/*
		 * If things still don't work, then default to assuming 
		 * everything is a potential iocsh function
		 */
		if (temp_help == NULL) { return true; }
	}
	
	FILE* prev = epicsGetThreadStdout();
	
	epicsSetThreadStdout(temp_help);
	iocshCmd("help()");
	epicsSetThreadStdout(prev);
	
	long size = ftell(temp_help);
	rewind(temp_help);
	char* buffer = (char*) malloc(sizeof(char) * size);
	
	fread(buffer, 1, size, temp_help);
	std::stringstream help_str;
	
	help_str.str(buffer);
	free(buffer);
	
	rewind(temp_help);
	
	std::string line;
	std::string element;

	int skipped_first_line = 0;
	
	while (std::getline(help_str, line, '\n'))
	{
		if (! skipped_first_line)
		{
			skipped_first_line = 1;
			continue;
		}
		
		std::stringstream check_line;
		check_line.str(line);
		
		while (std::getline(check_line, element, ' '))
		{
			if(! element.empty() && element == func_name)
			{
				return true;
			}
		}
	}
			
	return false;		
}


static int l_iocindex(lua_State* state)
{
	const char* symbol_name = lua_tostring(state, 2);
	
	if (std::string(symbol_name) == "exit")
	{
		lua_pushlightuserdata(state, NULL);
		return lua_error(state);
	}
	
	std::stringstream environ_check;
	
	environ_check << "return (os.getenv('";
	environ_check << symbol_name;
	environ_check << "'))";
	
	luaL_dostring(state, environ_check.str().c_str());
	
	if (! lua_isnil(state, -1)) { return 1; }
	
	lua_pop(state, 1);
	
	if (! parseHelp(symbol_name)) { return 0; }
	
	static const luaL_Reg func_meta[] = {
		{"__call", l_call},
		{NULL, NULL}
	};
	
	luaL_newmetatable(state, "func_meta");
	luaL_setfuncs(state, func_meta, 0);
	lua_pop(state, 1);

	lua_newtable(state);
	luaL_setmetatable(state, "func_meta");

	lua_pushstring(state, symbol_name);
	lua_setfield(state, -2, "function_name");

	return 1;
}

static int l_iochash_enable(lua_State* state)
{
	lua_pushstring(state, "YES");
	lua_setglobal(state, "enableHashComments");

	lua_pushstring(state, "Accepting iocsh-style comments");
	
	return 1;
}


int luaopen_iocsh (lua_State* state)
{
	static const luaL_Reg iocsh_meta[] = {
		{"__index", l_iocindex},
		{NULL, NULL}  /* sentinel */
	};

	static const luaL_Reg iocsh_funcs[] = {
		{NULL, NULL}
	};
	
	static const luaL_Reg hash_meta[] = {
		{"__len", l_iochash_enable},
		{NULL, NULL}
	};

	luaL_newmetatable(state, "iocsh_meta");
	luaL_setfuncs(state, iocsh_meta, 0);
	lua_pop(state, 1);
	
	luaL_newmetatable(state, "hash_enable_meta");
	luaL_setfuncs(state, hash_meta, 0);
	lua_pop(state, 1);

	luaL_newlib(state, iocsh_funcs);
	luaL_setmetatable(state, "iocsh_meta");

	lua_newtable(state);
	luaL_setmetatable(state, "hash_enable_meta");
	lua_setglobal(state, "ENABLE_HASH_COMMENTS");
	
	return 1;
}

epicsShareFunc lua_State* luaCreateState()
{
	lua_State* output = luaL_newstate();
	luaL_openlibs(output);
	luaLoadRegistered(output);

	luaL_requiref(output, "iocsh", luaopen_iocsh, 1);
	lua_pop(output, 1);

	return output;
}

epicsShareFunc lua_State* luaNamedState(const char* name)
{
	if (! name) { return NULL; }
	
	std::string state_name(name);
	
	if (named_states.find(state_name) != named_states.end())
	{
		return named_states[state_name];
	}
	
	lua_State* output = luaCreateState();

	named_states[state_name] = output;
	
	return output;
}
