Apache module for websockets using EezzServer

Configuration in httpd.conf with following assumptions:

The module is registered for files which ends on ".eezz"
Python is installed in the root directory /python32
The EezzServer applications are located in DocumentRoot /apache/httpdoc/webroot/applications/EezzServer

If you omit WsHostname in the configuration the hostname is fetched from httpd

----- httpd.conf ----------------------------

DocumentRoot /apache/httpdoc/webroot/public

<Directory "/apache/httpdoc/webroot/public">
# Specify your acces on the document root
</Directory>

<Files ~ "\.(eezz)$"> 
	PythonPath "/Python34:/Python34/DLLs:/Python34/DLLs:/Python34/lib:/Python34/site-packages:/apache/httpdoc/webroot/applications/EezzServer"
	Websocket  8401
	WsHostname localhost
	SetHandler eezz_websocket
</Files>

----- httpd.conf ----------------------------


The Project:
The Python-API runs in one dedicated thread. Its not possible to assign any variable or any method call from
different threads. 

Another restriction is, that suspending this thread using C++ mutex would also suspend all Python child threads.
The Websocket implementation has many threads and would not work.

In the design all Python-API is gathered in a single function, which is then threaded.
The communication between Apache handler and Python-API is done via global configuration.

The synchronization between the handler and the Python thread is done with condition 
aReadyMtx and predicate aPythonBusy

The Python module websocket implements the class TWakeup, which could be triggered on connect 
socket ('localhost', 6300)

A connect would trigger the mutex TWakeup.aCvExtern, which is used to suspend the Python main thread



 
