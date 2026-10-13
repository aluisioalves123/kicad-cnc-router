#!/bin/sh
# Compila o nucleo. Opcional no sentido de que o Python recusa binario de outra
# ABI - mas nao no sentido de desempenho: sem ele nao ha roteador.
set -e
cd "$(dirname "$0")"
case "$(uname -s 2>/dev/null || echo Windows)" in
  MINGW*|MSYS*|CYGWIN*|Windows*)alvo=perfcnc.dll; extra="" ;;
  *)                            alvo=perfcnc.so;  extra="-fPIC" ;;
esac
gcc -O2 -Wall -Wextra -std=c99 -shared $extra -o "$alvo" grade.c roteador.c projeto.c posicionador.c -lm
echo "compilado: $alvo"
