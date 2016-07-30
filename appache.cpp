// ------------------------------------------------------------
// appache.cpp : Definiert den Einstiegspunkt für die Konsolenanwendung.
//
// Copyright(C) 2015  Albert Zedlitz
//
// This program is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program.If not, see <http://www.gnu.org/licenses/>.
// ------------------------------------------------------------
// #define AP_HAVE_DESIGNATED_INITIALIZER

// #include <iostream>
#include <stdio.h>
#include <string>
#include <thread>
#include <mutex>
#include <locale>
#include <codecvt>
#include <direct.h>
#include <exception>
#include <hash_map>
#include <sstream>
#include <stdio.h>
#include <string.h>

#include "apr_hash.h"
#include "ap_config.h"
#include "ap_provider.h"
#include "httpd.h"
#include "http_core.h"
#include "http_config.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"

#undef _DEBUG
#include <Python.h>

using namespace std;
static   mutex  aMutex;
static   mutex  aPythonMtx;
typedef  hash_map<string, PyObject*> TPyHash;

// ------------------------------------------------------------
// ------------------------------------------------------------
typedef struct {
    vector<string> mPathList;
    int            mWebsocket;
    string         mHostname;
} TEezzConfig;

// ------------------------------------------------------------
// Takes a mutex and tries to lock it. I would also accept a
// mutex which is already locked. 
// Unlocks the given mutex at the end of the scope.
// ------------------------------------------------------------
class TGuard {
private:
    mutex *mMutex;
public:
    TGuard(mutex *aMutex) {
        mMutex = aMutex;
        mMutex->try_lock();
    }
    ~TGuard() {
        mMutex->unlock();
    }
};

// ------------------------------------------------------------
// ------------------------------------------------------------
class TGuardObjects {
private:
    TPyHash *aPythonObjects;
public:
    TGuardObjects(TPyHash *aObjects) {
        aPythonObjects = aObjects;
    }
    ~TGuardObjects() {
        for (auto xEntry : *aPythonObjects) {
            Py_XDECREF(xEntry.second);
        }
    }
};

static TEezzConfig mConfig;

// ------------------------------------------------------------
// ------------------------------------------------------------
const char *setPythonPath(cmd_parms *cmd, void *cfg, const char *arg) {
    istringstream aPath(arg);
    string        aSegment;

    while (std::getline(aPath, aSegment, ':')) {
        mConfig.mPathList.push_back(aSegment);
    }
    return NULL;
}

// ------------------------------------------------------------
// ------------------------------------------------------------
const char *setWebsocket(cmd_parms *cmd, void *cfg, const char *arg) {
    mConfig.mWebsocket = atoi(arg);
    return NULL;
}

// ------------------------------------------------------------
// ------------------------------------------------------------
const char *setHostname(cmd_parms *cmd, void *cfg, const char *arg) {
    mConfig.mHostname = arg;
    return NULL;
}

// ------------------------------------------------------------
// ------------------------------------------------------------
static const command_rec directives[] = {
    AP_INIT_TAKE1("PythonPath", reinterpret_cast<cmd_func>(setPythonPath), NULL, ACCESS_CONF, "set the python path"),
    AP_INIT_TAKE1("Websocket",  reinterpret_cast<cmd_func>(setWebsocket),  NULL, ACCESS_CONF, "set websocket port"),
    AP_INIT_TAKE1("WsHostname", reinterpret_cast<cmd_func>(setHostname),   NULL, ACCESS_CONF, "set websocket host"),
    { NULL }
};

// ------------------------------------------------------------
// ------------------------------------------------------------
extern "C" static void register_hooks(apr_pool_t *pool);
extern "C" module AP_MODULE_DECLARE_DATA eezz_websocket_module = {
    STANDARD20_MODULE_STUFF,
    NULL,
    NULL,
    NULL,
    NULL,
    directives,
    register_hooks
};


// ------------------------------------------------------------
// Define an exception to interrupt Python processing
// and decrement reference to allocated variables
// ------------------------------------------------------------
class TPyExcept : public exception {
    string mMessage;
public:
    TPyExcept(string aMessage) {
        mMessage = aMessage;
    }
    virtual const char* what() const throw() {
        return mMessage.c_str();
    }
};

// ------------------------------------------------------------
// startWebSocket
// Prevent from starting twice using mutex
//
// Configuation has to be read for
// host
// port
// webroot for python application
// ------------------------------------------------------------
void startWebSocket() {
    TPyHash aPyHashMap;

    if (aMutex.try_lock()) {
        // Release the mutex at the end of the block
        TGuard aMtxGuard(&aMutex);

        try {
            // Handle the reference count of PyObjects
            TGuardObjects aObjGuard(&aPyHashMap);
            
            // Start the web socket interface
            aPyHashMap["Import"]     = Py_BuildValue("s", "eezz.websocket");
            aPyHashMap["Module"]     = PyImport_Import(aPyHashMap["Import"]);

            if (aPyHashMap["Module"] == NULL) {
                throw TPyExcept("");
            }
            //ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "load eezz.websocket");
            aPyHashMap["TWebSocket"] = PyObject_GetAttrString(aPyHashMap["Module"], "TWebSocket");

            if (aPyHashMap["TWebSocket"] == NULL) {
                throw TPyExcept("");
            }

            aPyHashMap["Address"]    = Py_BuildValue("(si)", mConfig.mHostname.c_str(), mConfig.mWebsocket);
            aPyHashMap["Arguments"]  = Py_BuildValue("(S)", aPyHashMap["Address"]);
            aPyHashMap["WebSocket"]  = PyObject_CallObject(aPyHashMap["TWebSocket"], aPyHashMap["Arguments"]);

            if (aPyHashMap["WebSocket"] == NULL) {
                throw TPyExcept("");
            }

            aPyHashMap["ResultStart"] = PyObject_CallMethod(aPyHashMap["WebSocket"], "start", NULL);
            // aPyHashMap["ResultJoin"]  = PyObject_CallMethod(aPyHashMap["WebSocket"], "join", NULL);
        }
        catch (TPyExcept& xEx) {
            // ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server, "Failed to load websocket");
            PyErr_Print();
        }
    }
}

// ------------------------------------------------------------
// Next steps:
// configure sys path to the distribution
// configure python path
// ------------------------------------------------------------
extern "C" static int eezz_handler(request_rec *r) {
    // Initialize Python runtime
    TPyHash aPyHashMap;
    int     aReturn = OK;
    ostringstream aDocRoot;
    ostringstream aAppRoot;

    if (!r->handler || strcmp(r->handler, "eezz_websocket")) 
        return (DECLINED);
    
    // TEezzConfig *aConfig = (TEezzConfig *)ap_get_module_config(r-> r->per_dir_config, &eezz_websocket_module);

    aDocRoot << r->htaccess->dir << "/public" << ends;
    aAppRoot << r->htaccess->dir << "/applications/EezzServer" << ends;

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "eezz_handler");
    
    // Handle the reference count of PyObjects
    TGuardObjects aObjGuard(&aPyHashMap);

    if (aPythonMtx.try_lock()) {
        string aFilePath(r->canonical_filename);
        size_t aPos  = aFilePath.find("webroot");
        ostringstream aPath;
        Py_SetProgramName(L"eezz_websocket_handler");
        Py_Initialize();
        
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "initialize %s", aPath.str().c_str());
        aPyHashMap["sys.Import"]   = Py_BuildValue("s", "sys");
        aPyHashMap["sys.Module"]   = PyImport_Import(aPyHashMap["sys.Import"]);
        aPyHashMap["sys.path"]     = PyObject_GetAttrString(aPyHashMap["sys.Module"], "path");
        
        for (string aSegment : mConfig.mPathList) {
            PyObject_CallMethod( aPyHashMap["sys.path"], "append", "s", aSegment.c_str() );
        }

        //aPyHashMap["sys.path.ref"] = PyObject_CallMethod(aPyHashMap["sys.path"], "extend", "[sssss]", 
        //    "C:\\Python34\\DLLs", 
        //    "C:\\Python34\\lib", 
        //    "C:\\Python34", 
        //    "C:\\Python34\\lib\\site-packages",
        //    "C:\\Users\\Paul\\gitdev\\eezzgit\\webroot\\applications\\EezzServer");

        // Start web socket
        if (mConfig.mHostname.length() < 1) {
            mConfig.mHostname = r->hostname;
        }
        startWebSocket();
    }
    
    // Start the agent
    try {
        aPyHashMap["Import"]     = Py_BuildValue("s", "eezz.agent");
        aPyHashMap["Module"]     = PyImport_Import(aPyHashMap["Import"]);

        if (aPyHashMap["Module"] == NULL) {
            throw TPyExcept("");
        }
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "load eezz.agent");

        aPyHashMap["DocRoot"]    = Py_BuildValue("s", aDocRoot.str().c_str());
        aPyHashMap["Address"]    = Py_BuildValue("(si)", "localhost", 8100);
        aPyHashMap["Arguments"]  = Py_BuildValue("(SS)", aPyHashMap["DocRoot"], aPyHashMap["Address"]);
        aPyHashMap["TEezzAgent"] = PyObject_GetAttrString(aPyHashMap["Module"], "TEezzAgent");
 
        if (aPyHashMap["TEezzAgent"] == NULL) {
            throw TPyExcept("");
        }
        aPyHashMap["Agent"]      = PyObject_CallObject(aPyHashMap["TEezzAgent"], aPyHashMap["Arguments"]);
        if (aPyHashMap["Agent"] == NULL) {
            throw TPyExcept("");
        }
        aPyHashMap["ResultHdle"] = PyObject_CallMethod(aPyHashMap["Agent"], "handle_request", "(sSiS)", r->filename, Py_None, 0, Py_None);

        // Send the result
        Py_ssize_t aSize;
        wchar_t *aStringRes = PyUnicode_AsWideCharString(aPyHashMap["ResultHdle"], &aSize);
        wstring_convert<codecvt_utf8<wchar_t>> xConvert;
        string aAsciiResult = xConvert.to_bytes(aStringRes);

        ap_set_content_type(r, "text/html");
        ap_rprintf(r, aAsciiResult.c_str());

        PyMem_Free(aStringRes);
        aPyHashMap["ResultShut"] = PyObject_CallMethod(aPyHashMap["Agent"], "shutdown", NULL);
    }
    catch (TPyExcept& xEx) {
        PyErr_Print();
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "Failed to load agent");
        aReturn = HTTP_INTERNAL_SERVER_ERROR;
    }    
    return aReturn;
}

// ------------------------------------------------------------
// ------------------------------------------------------------
extern "C" static void register_hooks(apr_pool_t *pool) {
    ap_hook_handler(eezz_handler, NULL, NULL, APR_HOOK_LAST);
}


// ------------------------------------------------------------
// ------------------------------------------------------------
int main(int argc, char* argv[]) {
	return 0;
}

