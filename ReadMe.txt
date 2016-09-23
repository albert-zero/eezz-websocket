
Apache module for websockets using EezzServer

Configuration in httpd.conf with following assumptions:

The module is registered for files which ends on ".eezz"
EezzServer runs on a given local port

The mod_websocket would work as proxy and implements all details of 
the websocket protocol. The stripped protocoll is called peezz.

- Input is a JSON string as described in EezzServer document. 
- The application has to fill the update structure of this JSON 
  structure together with the appended 'return' sub structure.
  
----- httpd.conf ----------------------------

DocumentRoot /apache/httpdoc/webroot/public

<Directory "/apache/httpdoc/webroot/public">
# Specify your acces on the document root
</Directory>

<Files ~ "\.(eezz)$"> 
	Websocket  8401
</Files>

----- httpd.conf ----------------------------




 
