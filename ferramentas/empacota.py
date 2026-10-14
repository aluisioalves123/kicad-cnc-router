# -*- coding: utf-8 -*-
"""Monta o pacote do Plugin and Content Manager (PCM) do KiCad.

Gera em dist/ o .zip que o PCM instala e o arquivo de recursos (icone), e em pcm/
o packages.json e o repository.json - que fazem do proprio GitHub um repositorio
de plugins: no KiCad, "Gerenciar repositorios" e cola a URL do repository.json.

Os nucleos em C precisam estar compilados antes (nucleo/build.sh no Windows e no
Linux): o pacote leva o .dll e o .so, e o plugin carrega o do sistema em que roda.

    python ferramentas/empacota.py
"""
import hashlib
import json
import os
import sys
import time
import zipfile

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSAO = "1.4.0"
IDENTIFICADOR = "com.github.aluisioalves123.cncrouter"
REPO = "aluisioalves123/kicad-cnc-router"
PAGINA = "https://github.com/" + REPO
BRUTO = "https://raw.githubusercontent.com/%s/main/pcm/" % REPO
BAIXAR = "https://github.com/%s/releases/download/v%s/" % (REPO, VERSAO)

AUTOR = {"name": "Aluisio Cunha",
         "contact": {"github": "https://github.com/aluisioalves123",
                     "web": "https://hardware.aluisiocunha.com.br"}}

PACOTE = {
    "$schema": "https://go.kicad.org/pcm/schemas/v1",
    "name": "Roteador CNC - fresagem caseira",
    "description": "Posiciona e roteia placas para fresar em casa: trilha a 45 graus, "
                   "nada de solda do lado dos componentes, vias soldaveis e areas "
                   "proibidas respeitadas.",
    "description_full":
        "Posiciona as pecas e roteia a placa pensando em quem vai FRESAR e SOLDAR "
        "na bancada, nao em fabrica:\n\n"
        "- nenhuma trilha chega em pad pela face de cima: antes do pad ela desce "
        "por uma via e chega por baixo, entao toda solda e do lado da solda;\n"
        "- trilhas a 45 graus, com poucas curvas;\n"
        "- via com a mesma broca e a mesma ilha dos pads (furo comum, soldavel);\n"
        "- folgas do projeto (classe de rede e cobre-borda) e areas proibidas "
        "quaisquer;\n"
        "- pecas travadas no cadeado do KiCad nao se movem.\n\n"
        "O posicionador sorteia posicoes em paralelo e usa o proprio roteador como "
        "juiz, ate fechar a placa inteira ou acabar o tempo. Nada e aplicado sem "
        "voce ver o resultado e confirmar, e Ctrl+Z desfaz.\n\n"
        "English: placement and routing for home CNC-milled PCBs - 45-degree "
        "traces, no top-side pad connections (a via drops the trace to the "
        "bottom before each pad), solderable vias, keepouts and project "
        "clearances respected. Core written in C. UI in Portuguese.",
    "identifier": IDENTIFICADOR,
    "type": "plugin",
    "author": AUTOR,
    "maintainer": AUTOR,
    "license": "MIT",
    "resources": {"Homepage": PAGINA},
    "tags": ["routing", "autorouter", "placement", "cnc", "milling", "pcb"],
}
VERSAO_INFO = {"version": VERSAO, "status": "stable", "kicad_version": "10.0",
               "runtime": "swig", "platforms": ["windows", "linux"]}

# o que vai para dentro de plugins/ no pacote, relativo a raiz do projeto
ARQUIVOS = ["__init__.py", "interface.py", "icone.png", "icone_escuro.png", "LICENSE"]
PASTAS = {"cnc_router": (".py",), "nucleo": (".c", ".h", ".sh", ".dll", ".so")}
BINARIOS = ["nucleo/perfcnc.dll", "nucleo/perfcnc.so"]


def _sha(caminho):
    h = hashlib.sha256()
    with open(caminho, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def _lista():
    itens = list(ARQUIVOS)
    for pasta, exts in PASTAS.items():
        for nome in sorted(os.listdir(os.path.join(RAIZ, pasta))):
            if nome.endswith(exts):
                itens.append(pasta + "/" + nome)
    return itens


def principal():
    faltam = [b for b in BINARIOS if not os.path.exists(os.path.join(RAIZ, b))]
    if faltam:
        print("faltam nucleos compilados: %s (rode nucleo/build.sh no Windows e no "
              "Linux)" % ", ".join(faltam))
        return 1

    dist = os.path.join(RAIZ, "dist")
    os.makedirs(dist, exist_ok=True)
    metadata = dict(PACOTE, versions=[dict(VERSAO_INFO)])

    nome_zip = "%s-%s.zip" % (IDENTIFICADOR.replace(".", "_"), VERSAO)
    zip_pacote = os.path.join(dist, nome_zip)
    instalado = 0
    with zipfile.ZipFile(zip_pacote, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("metadata.json", json.dumps(metadata, indent=2, ensure_ascii=False))
        for rel in _lista():
            z.write(os.path.join(RAIZ, rel), "plugins/" + rel)
            instalado += os.path.getsize(os.path.join(RAIZ, rel))
        z.write(os.path.join(RAIZ, "pcm", "icon.png"), "resources/icon.png")
        instalado += os.path.getsize(os.path.join(RAIZ, "pcm", "icon.png"))

    # recursos do repositorio: o icone que aparece na lista do PCM
    zip_recursos = os.path.join(dist, "resources.zip")
    with zipfile.ZipFile(zip_recursos, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(RAIZ, "pcm", "icon.png"), IDENTIFICADOR + "/icon.png")

    versao = dict(VERSAO_INFO,
                  download_url=BAIXAR + nome_zip,
                  download_sha256=_sha(zip_pacote),
                  download_size=os.path.getsize(zip_pacote),
                  install_size=instalado)
    pacotes = {"packages": [dict({k: v for k, v in PACOTE.items() if k != "$schema"},
                                 versions=[versao])]}
    caminho_pacotes = os.path.join(RAIZ, "pcm", "packages.json")
    with open(caminho_pacotes, "w", encoding="utf-8", newline="\n") as f:
        json.dump(pacotes, f, indent=2, ensure_ascii=False)
        f.write("\n")

    agora = int(time.time())
    utc = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(agora))
    repositorio = {
        "$schema": "https://go.kicad.org/pcm/schemas/v1#/definitions/Repository",
        "name": "Roteador CNC (aluisioalves123)",
        "maintainer": AUTOR,
        "packages": {"url": BRUTO + "packages.json", "sha256": _sha(caminho_pacotes),
                     "update_timestamp": agora, "update_time_utc": utc},
        "resources": {"url": BAIXAR + "resources.zip", "sha256": _sha(zip_recursos),
                      "update_timestamp": agora, "update_time_utc": utc},
    }
    with open(os.path.join(RAIZ, "pcm", "repository.json"), "w", encoding="utf-8",
              newline="\n") as f:
        json.dump(repositorio, f, indent=2, ensure_ascii=False)
        f.write("\n")

    print("pacote ........ %s (%d bytes, instala %d)"
          % (zip_pacote, os.path.getsize(zip_pacote), instalado))
    print("recursos ...... %s" % zip_recursos)
    print("repositorio ... pcm/repository.json + pcm/packages.json")
    print("suba %s e resources.zip na release v%s" % (nome_zip, VERSAO))
    return 0


if __name__ == "__main__":
    sys.exit(principal())
