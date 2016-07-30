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