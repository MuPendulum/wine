/*
 * Copyright 2016 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

var xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<a name=\"test\">wine</a>";

function test_xhr() {
    var xhr = new XMLHttpRequest();
    var complete_cnt = 0, loadstart = false;
    var v = document.documentMode;

    xhr.onreadystatechange = function() {
        if(xhr.readyState != 4)
            return;

        ok(xhr.responseText === xml, "unexpected responseText " + xhr.responseText);
        ok(xhr.responseXML !== null, "unexpected null responseXML");

        var x = xhr.responseXML, r = Object.prototype.toString.call(x);
        ok(r === (v < 10 ? "[object Object]" : (v < 11 ? "[object Document]" : "[object XMLDocument]")),
                "XML document Object.toString = " + r);

        r = Object.getPrototypeOf(x);
        if(v < 10)
            ok(r === null, "prototype of returned XML document = " + r);
        else if(v < 11)
            ok(r === window.Document.prototype, "prototype of returned XML document = " + r);
        else
            ok(r === window.XMLDocument.prototype, "prototype of returned XML document" + r);

        if(v < 10) {
            ok(!("anchors" in x), "anchors is in returned XML document");
            ok(Object.prototype.hasOwnProperty.call(x, "createElement"), "createElement not a prop of returned XML document");
        }else {
            ok("anchors" in x, "anchors not in returned XML document");
            ok(!x.hasOwnProperty("createElement"), "createElement is a prop of returned XML document");
            r = x.anchors;
            ok(r.length === 0, "anchors.length of returned XML document = " + r.length);
        }

        if(complete_cnt++ && !("onloadend" in xhr))
            next_test();
    }
    xhr.ontimeout = function() { ok(false, "ontimeout called"); }
    var onload_func = xhr.onload = function() {
        ok(xhr.statusText === "OK", "statusText = " + xhr.statusText);
        if("onloadstart" in xhr)
            ok(loadstart, "onloadstart not fired");
        if(complete_cnt++ && !("onloadend" in xhr))
            next_test();
    };
    ok(xhr.onload === onload_func, "xhr.onload != onload_func");
    if("onloadstart" in xhr) {
        xhr.onloadstart = function(e) {
            ok(complete_cnt == 0, "onloadstart fired after onload");
            var props = [ "initProgressEvent", "lengthComputable", "loaded", "total" ];
            for(var i = 0; i < props.length; i++)
                ok(props[i] in e, props[i] + " not available in loadstart");
            ok(e.lengthComputable === false, "lengthComputable in loadstart = " + e.lengthComputable);
            ok(e.loaded === 0, "loaded in loadstart = " + e.loaded);
            ok(e.total === 18446744073709552000, "total in loadstart = " + e.total);
            loadstart = true;
        };
        xhr.onloadend = function(e) {
            ok(complete_cnt == 2, "onloadend not fired after onload and onreadystatechange");
            ok(loadstart, "onloadstart not fired before onloadend");
            var props = [ "initProgressEvent", "lengthComputable", "loaded", "total" ];
            for(var i = 0; i < props.length; i++)
                ok(props[i] in e, props[i] + " not available in loadend");
            ok(e.lengthComputable === true, "lengthComputable in loadend = " + e.lengthComputable);
            ok(e.loaded === xml.length, "loaded in loadend = " + e.loaded);
            ok(e.total === xml.length, "total in loadend = " + e.total);
            next_test();
        };
    }

    xhr.open("POST", "echo.php", true);
    xhr.setRequestHeader("X-Test", "True");
    if("withCredentials" in xhr) {
        ok(xhr.withCredentials === false, "default withCredentials = " + xhr.withCredentials);
        xhr.withCredentials = true;
        ok(xhr.withCredentials === true, "withCredentials = " + xhr.withCredentials);
        xhr.withCredentials = false;
    }
    xhr.send(xml);
}

function test_sync_xhr() {
    var async_xhr, async_xhr2, sync_xhr, sync_xhr_in_async, sync_xhr_nested, a = [ 0 ];
    var async_xhr_clicked = false, doc_dblclicked = false;
    function onmsg(e) { a.push("msg" + e.data); }
    document.ondblclick = function() { doc_dblclicked = true; };
    window.addEventListener("message", onmsg);
    window.postMessage("1", "*");
    window.setTimeout(function() { a.push("timeout"); }, 0);
    window.postMessage("2", "*");
    a.push(1);

    async_xhr = new XMLHttpRequest();
    async_xhr.open("POST", "echo.php", true);
    async_xhr.onreadystatechange = function() {
        if(async_xhr.readyState < 3)
            return;
        a.push("async_xhr(" + async_xhr.readyState + ")");
        ok(async_xhr2.readyState === 1, "async_xhr2.readyState = " + async_xhr2.readyState);
        if(async_xhr.readyState == 4) {
            window.postMessage("_async", "*");

            sync_xhr_in_async = new XMLHttpRequest();
            sync_xhr_in_async.open("POST", "echo.php", false);
            sync_xhr_in_async.onreadystatechange = function() { if(sync_xhr_in_async.readyState == 4) a.push("sync_xhr_in_async"); };
            sync_xhr_in_async.setRequestHeader("X-Test", "True");
            sync_xhr_in_async.send("sync_in_async");
        }
    };
    async_xhr.addEventListener("click", function() { async_xhr_clicked = true; });
    async_xhr.setRequestHeader("X-Test", "True");
    async_xhr.send("1234");
    a.push(2);

    async_xhr2 = new XMLHttpRequest();
    async_xhr2.open("POST", "echo.php?delay_with_signal", true);
    async_xhr2.onreadystatechange = function() {
        if(async_xhr2.readyState < 3)
            return;
        a.push("async_xhr2(" + async_xhr2.readyState + ")");
        ok(async_xhr.readyState === 4, "async_xhr.readyState = " + async_xhr.readyState);
    };
    async_xhr2.setRequestHeader("X-Test", "True");
    async_xhr2.send("foobar");
    a.push(3);

    sync_xhr = new XMLHttpRequest();
    sync_xhr.open("POST", "echo.php?delay_with_signal", false);
    sync_xhr.onreadystatechange = function() {
        a.push("sync_xhr(" + sync_xhr.readyState + ")");
        ok(async_xhr.readyState === 1, "async_xhr.readyState in sync_xhr handler = " + async_xhr.readyState);
        ok(async_xhr2.readyState === 1, "async_xhr2.readyState in sync_xhr handler = " + async_xhr2.readyState);
        if(sync_xhr.readyState < 4)
            return;
        window.setTimeout(function() { a.push("timeout_sync"); }, 0);
        window.postMessage("_sync", "*");

        sync_xhr_nested = new XMLHttpRequest();
        sync_xhr_nested.open("POST", "echo.php", false);
        sync_xhr_nested.onreadystatechange = function() {
            a.push("nested(" + sync_xhr_nested.readyState + ")");
            if(sync_xhr_nested.readyState == 4) {
                window.setTimeout(function() { a.push("timeout_nested"); }, 0);
                window.postMessage("_nested", "*");

                var e = document.createEvent("Event");
                e.initEvent("click", true, false);
                ok(async_xhr_clicked === false, "async_xhr click fired before dispatch");
                async_xhr.dispatchEvent(e);
                ok(async_xhr_clicked === true, "async_xhr click not fired immediately");
                if(document.fireEvent) {
                    ok(doc_dblclicked === false, "document dblclick fired before dispatch");
                    document.fireEvent("ondblclick", document.createEventObject());
                    ok(doc_dblclicked === true, "document dblclick not fired immediately");
                }
            }
        };
        sync_xhr_nested.setRequestHeader("X-Test", "True");
        sync_xhr_nested.send("nested");
    };
    sync_xhr.setRequestHeader("X-Test", "True");
    sync_xhr.send("abcd");
    a.push(4);

    window.setTimeout(function() {
        var r = a.join(",");
        ok(r === "0,1,2,3," + (document.documentMode < 10 ? "sync_xhr(1),sync_xhr(2),sync_xhr(3)," : "") +
                 "sync_xhr(4)," + (document.documentMode < 10 ? "nested(1),nested(2),nested(3)," : "") +
                 "nested(4),4,async_xhr(3),async_xhr(4),sync_xhr_in_async,async_xhr2(3),async_xhr2(4)," +
                 "msg1,msg2,msg_sync,msg_nested,msg_async,timeout,timeout_sync,timeout_nested",
           "unexpected order: " + r);
        window.removeEventListener("message", onmsg);
        document.ondblclick = null;
        a = [ 0 ];

        // Events dispatched to other iframes are not blocked by a send() in another context,
        // except for async XHR events (which are a special case again), messages, and timeouts.
        var iframe = document.createElement("iframe"), iframe2 = document.createElement("iframe");
        iframe.onload = function() {
            iframe2.onload = function() {
                function onmsg(e) {
                    a.push(e.data);
                    if(e.data === "echo")
                        iframe2.contentWindow.postMessage("sync_xhr", "*");
                };

                window.setTimeout(function() {
                    var r = a.join(",");
                    ok(r === "0,1,async_xhr,echo,sync_xhr(pre-send),sync_xhr(DONE),sync_xhr,async_xhr(DONE)",
                       "[iframes 1] unexpected order: " + r);
                    a = [ 0 ];

                    window.setTimeout(function() {
                        var r = a.join(",");
                        ok(r === "0,1,echo,blank(DONE),sync_xhr(pre-send),sync_xhr(DONE),sync_xhr",
                           "[iframes 2] unexpected order: " + r);
                        window.removeEventListener("message", onmsg);
                        next_test();
                    }, 0);

                    iframe.onload = function() { a.push("blank(DONE)"); };
                    iframe.src = "blank.html?delay_with_signal";
                    iframe2.contentWindow.postMessage("echo", "*");
                    a.push(1);
                }, 0);

                window.addEventListener("message", onmsg);
                iframe.contentWindow.postMessage("async_xhr", "*");
                iframe2.contentWindow.postMessage("echo", "*");
                a.push(1);
            };
            iframe2.src = "xhr_iframe.html";
            document.body.appendChild(iframe2);
        };
        iframe.src = "xhr_iframe.html";
        document.body.appendChild(iframe);
    }, 0);
}

function test_content_types() {
    var xhr = new XMLHttpRequest(), types, i = 0, override = false;
    var v = document.documentMode;

    var types = [
        "",
        "text/plain",
        "text/html",
        "wine/xml",
        "xml"
    ];
    var xml_types = [
        "text/xmL",
        "apPliCation/xml",
        "application/xHtml+xml",
        "image/SvG+xml",
        "Wine/Test+xml",
        "++Xml",
        "+xMl"
    ];

    function onload() {
        ok(xhr.responseText === xml, "unexpected responseText " + xhr.responseText);
        if(v < 10 || types === xml_types) {
            ok(xhr.responseXML !== null, "unexpected null responseXML for " + types[i]);
            if(v >= 10) {
                var r = xhr.responseXML.mimeType, e = "text/xml";
                if(types[i] === "application/xHtml+xml" || types[i] === "image/SvG+xml")
                    e = types[i].toLowerCase();
                e = external.getExpectedMimeType(e);
                ok(r === e, "XML document mimeType for " + types[i] + " = " + r + ", expected " + e);
            }
        }else
            ok(xhr.responseXML === null, "unexpected non-null responseXML for " + (override ? "overridden " : "") + types[i]);

        if(("overrideMimeType" in xhr) && !override) {
            override = true;
            xhr = new XMLHttpRequest();
            xhr.onload = onload;
            xhr.open("POST", "echo.php", true);
            xhr.setRequestHeader("X-Test", "True");
            xhr.overrideMimeType(types[i]);
            xhr.send(xml);
            return;
        }
        override = false;

        if(++i >= types.length) {
            if(types === xml_types) {
                next_test();
                return;
            }
            types = xml_types;
            i = 0;
        }
        xhr = new XMLHttpRequest();
        xhr.onload = onload;
        xhr.open("POST", "echo.php?content-type=" + types[i], true);
        xhr.setRequestHeader("X-Test", "True");
        xhr.send(xml);
    }

    xhr.onload = onload;
    xhr.open("POST", "echo.php?content-type=" + types[i], true);
    xhr.setRequestHeader("X-Test", "True");
    xhr.send(xml);
}

function test_xdr() {
    if(!window.XDomainRequest) { next_test(); return; }

    var xdr = new XDomainRequest();
    xdr.open("POST", "echo.php");
    // send on native aborts with custom pluggable protocol handler even with the right
    // response headers (`XDomainRequestAllowed: 1` and `Access-Control-Allow-Origin: *`).

    // Only http/https schemes are allowed, and it must match with the origin's scheme
    xdr = new XDomainRequest();
    xdr.open("GET", "http://www.winehq.org/");

    xdr = new XDomainRequest();
    try {
        xdr.open("GET", "https://www.winehq.org/");
        ok(false, "xdr scheme mismatch did not throw exception");
    }catch(ex) {
        var n = ex.number >>> 0;
        ok(n === 0x80070005, "xdr scheme mismatch threw " + n);
    }
    next_test();
}

function test_abort() {
    var xhr = new XMLHttpRequest();
    if(!("onabort" in xhr)) { next_test(); return; }

    xhr.onreadystatechange = function() {
        if(xhr.readyState != 4)
            return;
        todo_wine_if(v < 10).
        ok(v >= 10, "onreadystatechange called");
    }
    xhr.onload = function() { ok(false, "onload called"); }
    xhr.onabort = function(e) { next_test(); }

    xhr.open("POST", "echo.php?delay", true);
    xhr.setRequestHeader("X-Test", "True");
    xhr.send("Abort Test");
    xhr.abort();
}

function test_timeout() {
    var xhr = new XMLHttpRequest();
    var v = document.documentMode;

    xhr.onreadystatechange = function() {
        if(xhr.readyState != 4)
            return;
        todo_wine_if(v < 10).
        ok(v >= 10, "onreadystatechange called");
    }
    xhr.onload = function() { ok(false, "onload called"); }
    xhr.ontimeout = function(e) {
        var r = Object.prototype.toString.call(e);
        ok(r === ("[object " + (v < 10 ? "Event" : "ProgressEvent") + "]"), "Object.toString = " + r);
        var props = [ "initProgressEvent", "lengthComputable", "loaded", "total" ];
        for(r = 0; r < props.length; r++) {
            if(v < 10)
                ok(!(props[r] in e), props[r] + " is available");
            else
                ok(props[r] in e, props[r] + " not available");
        }
        if(v >= 10) {
            ok(e.lengthComputable === false, "lengthComputable = " + e.lengthComputable);
            ok(e.loaded === 0, "loaded = " + e.loaded);
            ok(e.total === 18446744073709552000, "total = " + e.total);

            e.initProgressEvent("timeout", false, false, true, 13, 42);
            ok(e.lengthComputable === false, "lengthComputable after initProgressEvent = " + e.lengthComputable);
            ok(e.loaded === 0, "loaded after initProgressEvent = " + e.loaded);
            ok(e.total === 18446744073709552000, "total after initProgressEvent = " + e.total);
        }
        next_test();
    }

    xhr.open("POST", "echo.php?delay", true);
    xhr.setRequestHeader("X-Test", "True");
    xhr.timeout = 10;
    xhr.send("Timeout Test");
}

function test_responseType() {
    var i, xhr = new XMLHttpRequest();
    if(!("responseType" in xhr)) { next_test(); return; }

    ok(xhr.responseType === "", "default responseType = " + xhr.responseType);
    try {
        xhr.responseType = "";
        ok(false, "setting responseType before open() did not throw exception");
    }catch(ex) {
        todo_wine.
        ok(ex.name === "InvalidStateError", "setting responseType before open() threw " + ex.name);
    }
    try {
        xhr.responseType = "invalid response type";
        ok(false, "setting invalid responseType before open() did not throw exception");
    }catch(ex) {
        todo_wine.
        ok(ex.name === "InvalidStateError", "setting invalid responseType before open() threw " + ex.name);
    }

    xhr.open("POST", "echo.php", true);
    xhr.setRequestHeader("X-Test", "True");
    ok(xhr.responseType === "", "default responseType after open() = " + xhr.responseType);

    var types = [ "text", "", "document", "arraybuffer", "blob", "ms-stream" ];
    for(i = 0; i < types.length; i++) {
        xhr.responseType = types[i];
        ok(xhr.responseType === types[i], "responseType = " + xhr.responseType + ", expected " + types[i]);
    }

    types = [ "json", "teXt", "Document", "moz-chunked-text", "moz-blob", null ];
    for(i = 0; i < types.length; i++) {
        xhr.responseType = types[i];
        ok(xhr.responseType === "ms-stream", "responseType (after set to " + types[i] + ") = " + xhr.responseType);
    }

    xhr.responseType = "";
    xhr.onreadystatechange = function() {
        if(xhr.readyState < 3) {
            xhr.responseType = "";
            return;
        }
        try {
            xhr.responseType = "";
            ok(false, "setting responseType with state " + xhr.readyState + " did not throw exception");
        }catch(ex) {
            todo_wine.
            ok(ex.name === "InvalidStateError", "setting responseType with state " + xhr.readyState + " threw " + ex.name);
        }
    }
    xhr.onloadend = function() { next_test(); }
    xhr.send("responseType test");
}

function test_response() {
    var xhr = new XMLHttpRequest(), i = 0;
    if(!("response" in xhr)) { next_test(); return; }

    var types = [
        [ "text", "application/octet-stream", function() {
            if(xhr.readyState < 3)
                ok(xhr.response === "", "response for text with state " + state + " = " + xhr.response);
            else if(xhr.readyState === 4)
                ok(xhr.response === xml, "response for text = " + xhr.response);
        }],
        [ "arraybuffer", "image/png", function() {
            if(xhr.readyState < 4)
                ok(xhr.response === undefined, "response for arraybuffer with state " + state + " = " + xhr.response);
            else {
                var buf = xhr.response;
                ok(buf instanceof ArrayBuffer, "response for arraybuffer not instanceof ArrayBuffer");
                ok(buf.byteLength === xml.length, "response for arraybuffer byteLength = " + buf.byteLength);
                buf = new Uint8Array(buf);
                for(var i = 0; i < buf.length; i++) {
                    if(buf[i] !== xml.charCodeAt(i)) {
                        var a = new Array(buf.length);
                        for(var j = 0; j < a.length; j++) a[j] = buf[j];
                        ok(false, "response for arraybuffer is wrong (first bad char at pos " + i + "): " + a);
                        break;
                    }
                }
            }
        }],
        [ "blob", "wine/test", function() {
            if(xhr.readyState < 4)
                ok(xhr.response === undefined, "response for blob with state " + state + " = " + xhr.response);
        }]
    ];

    function onreadystatechange() {
        types[i][2]();
        if(xhr.readyState < 4)
            return;
        if(++i >= types.length) {
            next_test();
            return;
        }
        xhr = new XMLHttpRequest();
        xhr.open("POST", "echo.php?content-type=" + types[i][1], true);
        xhr.onreadystatechange = onreadystatechange;
        xhr.setRequestHeader("X-Test", "True");
        xhr.responseType = types[i][0];
        xhr.send(xml);
    }

    xhr.open("POST", "echo.php?content-type=" + types[i][1], true);
    xhr.onreadystatechange = onreadystatechange;
    xhr.setRequestHeader("X-Test", "True");
    xhr.responseType = types[i][0];
    xhr.send(xml);
}

var tests = [
    test_xhr,
    test_sync_xhr,
    test_xdr,
    test_content_types,
    test_abort,
    test_timeout,
    test_responseType,
    test_response
];
