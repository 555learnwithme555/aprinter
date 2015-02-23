{ stdenv, writeText, writeScriptBin, bash, coreutils, p7zip, rsync, unzip
, python27Packages, nix, ncd, lighttpd, aprinterSource
, serviceHost ? "127.0.0.1"
, servicePort ? 4000
, backendPort ? 4001
, stderrTruncateBytes ? 6000
}:
let
    gui_dist = stdenv.mkDerivation {
        name = "aprinter-gui-dist";
        unpackPhase = "true";
        nativeBuildInputs = [python27Packages.python unzip rsync];
        installPhase = ''
            python -B "${aprinterSource}"/config_system/gui/build.py --out-dir "$out"
        '';
    };
    
    lighttpd_proxy_config = ''
        $HTTP["url"] =~ "^/compile$" {
            proxy.server = ("" => (
                ("host" => "127.0.0.1", "port" => ${toString backendPort})
            ))
        }
    '';
    
    lighttpd_config = writeText "aprinter-service-lighttpd.cfg" ''
        server.modules += ("mod_proxy")
        
        server.document-root = "${gui_dist}" 

        server.bind = "${serviceHost}"
        server.port = ${toString servicePort}

        mimetype.assign = (
            ".html" => "text/html", 
            ".txt"  => "text/plain",
            ".jpg"  => "image/jpeg",
            ".png"  => "image/png",
            ".css"  => "text/css",
            ".js"   => "text/javascript"
        )

        index-file.names = ( "index.html" )
        
        ${lighttpd_proxy_config}
    '';
    
    ncd_script = withHttpServer: writeText "aprinter-service.ncd" ''
        include "${aprinterSource}/config_system/service/aprinter_compile_service.ncdi"
        
        process main {
            var("") temp_dir;
            Do {
                getenv("APRINTER_SERVICE_TMPDIR") aprinter_service_tmpdir;
                If (aprinter_service_tmpdir.exists) {
                    file_stat(aprinter_service_tmpdir) stat;
                    If (@not(stat.succeeded)) {
                        println("ERROR: The directory specified in APRINTER_SERVICE_TMPDIR does not exist.");
                        exit("1");
                    };
                    
                    temp_dir->set(aprinter_service_tmpdir);
                    _do->break();
                };
                
                getenv("HOME") home;
                If (home.exists) {
                    var(@concat(home, "/aprinter-service-temp")) test_temp;
                    file_stat(test_temp) stat;
                    If (stat.succeeded) {
                        temp_dir->set(test_temp);
                        _do->break();
                    };
                };
                
                println("ERROR: Cannot determine the temporary directory.");
                println("If you create $HOME/aprinter-service-temp, it will be used automatically.");
                println("Alternatively, you can set the APRINTER_SERVICE_TMPDIR environment variable.");
                exit("1");
            };
            
            var([
                "http_server": [
                    "listen_addr": {"ipv4", "127.0.0.1", "${toString backendPort}"},
                    "max_clients": "10",
                    "max_headers": "64",
                    "max_line_len": "512",
                    "max_request_payload_len": "262144",
                    "inactivity_timeout": "20000",
                    "server_name": "APrinter Compilation Service implemented in NCD"
                ],
                "max_concurrent_compiles": "1",
                "aprinter_src_dir": "${aprinterSource}",
                "temp_dir": temp_dir,
                "stderr_truncate_bytes": "${toString stderrTruncateBytes}",
                "mktemp": "${coreutils}/bin/mktemp",
                "rm": "${coreutils}/bin/rm",
                "python": "${python27Packages.python}/bin/python",
                "nixbuild": "${nix}/bin/nix-build",
                "7za": "${p7zip}/bin/7za",
                "mkdir": "${coreutils}/bin/mkdir",
                "rsync": "${rsync}/bin/rsync",
                "bash": "${bash}/bin/bash",
                "head": "${coreutils}/bin/head",
                "cat": "${coreutils}/bin/cat"
            ]) config;
            
            process_manager() mgr;
            mgr->start(@compile_service, {});
            ${if !withHttpServer then "" else "mgr->start(@static_content_service, {});"}
        }
        
        template compile_service {
            call(@aprinter_compile_service, {_caller.config});
        }
        
        ${if !withHttpServer then "" else ''
            template static_content_service {
                daemon({"${lighttpd}/sbin/lighttpd", "-D", "-f", "${lighttpd_config}"}, ["keep_stderr": @true]);
            }
        ''}
    '';
    
    executable = withHttpServer: writeScriptBin "aprinter-service" ''
        #!${bash}/bin/bash
        exec ${ncd}/bin/badvpn-ncd --loglevel notice ${ncd_script withHttpServer}
    '';

in
{
    inherit gui_dist;
    inherit lighttpd_proxy_config;
    withHttpServer = executable true;
    withoutHttpServer = executable false;
}
