with import <nixpkgs> {};
rec {
    gccAvrAtmel = pkgs.callPackage ./gcc_avr_atmel.nix {};
    
    asf = pkgs.callPackage ./asf.nix {};
    
    teensyCores = pkgs.callPackage ./teensy_cores.nix {};
    
    aprinterFunc = aprinterConfig: pkgs.callPackage ./aprinter.nix (
        {
            inherit gccAvrAtmel asf teensyCores;
        } // aprinterConfig
    );
    
    aprinterSymlinksFunc = aprinterBuilds: pkgs.callPackage ./symlinks.nix { inherit aprinterBuilds; };
    
    aprinterTestFunc = boardName: aprinterFunc {
        inherit boardName;
        buildName = "test-${boardName}";
        mainText = builtins.readFile ( ../main + "/aprinter-${boardName}.cpp" );
    };
    
    aprinterTestMelzi = aprinterTestFunc "melzi";
    aprinterTestRamps13 = aprinterTestFunc "ramps13";
    aprinterTestMegatronics3 = aprinterTestFunc "megatronics3";
    aprinterTestRampsfd = aprinterTestFunc "rampsfd";
    aprinterTestRadds = aprinterTestFunc "radds";
    aprinterTest4pi = aprinterTestFunc "4pi";
    aprinterTestMlab = aprinterTestFunc "mlab";
    aprinterTestTeensy3 = aprinterTestFunc "teensy3";
    
    aprinterTestAll = aprinterSymlinksFunc [
        aprinterTestMelzi aprinterTestRamps13 aprinterTestMegatronics3
        aprinterTestRampsfd aprinterTestRadds aprinterTest4pi
        aprinterTestMlab aprinterTestTeensy3
    ];
}
