set my_dir=%cd%
docker run --rm ^
        -v "%my_dir%:/build/chiaki":z ^
        -w "/build/chiaki" ^
        docker.io/xlanor/chiaki-ng-switch-builder:latest ^
        /bin/bash -c "scripts/switch/build.sh"
