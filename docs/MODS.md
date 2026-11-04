# Mods de arquivos

O launcher tem a aba **Mods**: chave geral, ativação de cada mod, setas de ordem de carga e um
painel com os detalhes do mod selecionado. As mudanças valem no próximo início. O mod mais abaixo na lista tem
prioridade; quando dois mods trocam o mesmo arquivo, o log mostra qual venceu.

## O que dá para mudar

- Texturas e modelos: personagens, roupas, armas, inimigos e cenários (WADs em `EXEC/WAD/PS4_LE/`)
- Interface: ícones de botão, menus e HUD
- Textos: menus e legendas (WADs de idioma, como `R_LANG_EN.WAD`)
- Áudio: falas (`.XVAG` nas pastas de idioma), música e efeitos (`EXEC/SOUNDS`)
- Vídeos: cinemáticas (`EXEC/CINEMATICS`)

## O que não dá

- Lógica do jogo (dano, física, IA, câmera): fica no executável. Use patches (pasta `patches/`)
- `eboot.bin`, `sce_module` e `sce_sys`: recusados pelo carregador
- Injetar DLL ou script
- Mesclar dois mods no mesmo arquivo: o último da lista vence
- Mod feito para outra região sem ajuste: um mod para `R_LANG_UK.WAD` não muda o `R_LANG_EN.WAD`

## Como montar um mod

A pasta padrão é `mods/` no diretório de dados. Cada mod fica na sua própria pasta e espelha a
árvore do jogo, que no WIPEOUTPC tem os dados em `EXEC/` e numa pasta por idioma
(`ENGLISH/`, `PORTUGUESE/`, ...):

```text
mods/
  Nome do mod/
    EXEC/WAD/PS4_LE/UI.WAD
    PORTUGUESE/SOUNDS/...
```

Pastas de outras regiões, como `JAPANESE/`, também são reconhecidas quando existem na raiz do
jogo selecionado. Isso vale tanto para a lista do launcher quanto para a aplicação dos mods.

Também são aceitos:

- `Nome do mod/dvdroot_ps4/EXEC/...` (convenção do shadPS4): o `dvdroot_ps4/` é descartado e o
  resto vai para a raiz do jogo;
- `Nome do mod/app0/EXEC/...`;
- uma pasta a mais por fora, como a de um arquivo extraído numa pasta com o nome dele:
  `Nome do mod/Nome do mod v1.2/EXEC/...` (readme e imagens ao lado são ignorados).

Na aba **Mods** do launcher, clicar no nome de um mod mostra os detalhes dele. Para preencher,
coloque na pasta do mod, ao lado de `EXEC/`, um `mod.json` e uma imagem `preview.png` (ou `.jpg`),
ambos opcionais:

```json
{ "name": "Neon Ship Livery", "author": "Seu nome", "version": "1.0",
  "description": "O que o mod muda.", "url": "https://..." }
```

Sem `mod.json`, o launcher usa o nome da pasta e o começo de um `readme.txt` ou `readme.md`. Sem
`preview`, usa a primeira imagem da pasta (`cover` e `screenshot` também têm prioridade). O link
só abre se começar com `http://` ou `https://`.

Pastas de idioma reconhecidas: `ENGLISH`, `FRENCH`, `GERMAN`, `ITALIAN`, `SPANISH`, `PORTUGUESE`,
`DUTCH`, `POLISH` e `RUSSIAN`. Um mod feito para outra região (por exemplo, o `R_LANG_UK.WAD` da
versão europeia) não troca nada num jogo que não tem esse arquivo: o log mostra `added` em vez de
`replaced`.

Maiúsculas e minúsculas nos nomes do mod não importam: um arquivo com outra caixa troca o
arquivo do jogo de mesmo nome. O log mostra quantos arquivos o mod trocou e quantos acrescentou:
`Mods: 12 game files replaced, 0 added`. Se o mod deveria trocar arquivos e aparece
`0 game files replaced`, confira os caminhos dentro dele. Um mod em **ZIP** pode ser largado direto na pasta de mods: ao abrir o launcher ou iniciar o jogo ele é extraído numa pasta com o nome do zip (uma pasta de mesmo nome que você criou não é substituída, e caminhos perigosos ou links no zip são recusados). Arquivos 7z/RAR precisam ser extraídos antes. Dois mods que trocam o mesmo arquivo não são mesclados: o último vence inteiro.

Uma pasta `<pasta do jogo>-mods/EXEC/...` ao lado da pasta do jogo (convenção do shadPS4)
entra antes dos mods da lista. A chave geral também a desliga.

Os arquivos do jogo nunca são alterados. A cada início é montada uma árvore de links em
`out/mod-game-*` no diretório de dados, apagada quando o jogo termina. `/app0` fica só para
leitura; os saves continuam na pasta de sempre.

Patches de código ficam em `patches/` / `WIPEOUT_PATCHES`. Links simbólicos dentro do mod são recusados.

Pelo terminal:

```sh
WIPEOUT_GAME_DIR=/caminho/CUSA07671 WIPEOUT_MODS_DIR=/caminho/mods python run.py
WIPEOUT_GAME_DIR=/caminho/CUSA07671 WIPEOUT_MODS_ENABLED=0 python run.py
```

Ordem e desativações ficam em `mods.json` no diretório de dados:

```json
{"order": ["Primeiro", "Segundo"], "disabled": ["Primeiro"]}
```

Pastas novas entram ativadas, no fim da lista, em ordem alfabética. `WIPEOUT_MODS_CONFIG` muda o
caminho desse arquivo.

## Mods com patches de jogo / Mods with game patches

Uma pasta de mod também pode trazer **patches de código** (para o `eboot.bin`): arquivos XML na pasta
`patches/` do mod, no mesmo formato dos patches embutidos (`patches/WipEout_Omega_Collection.xml`),
com assinaturas (`Pattern`/`At`) que acham cada ponto em qualquer build. Um mod pode ter só patches, sem
`dvdroot_ps4`:

```text
mods/
  Nome do mod/
    patches/
      meu-patch.xml
```

Os patches do mod valem enquanto o mod está ativado (a chave geral também os desliga) e entram junto com os
embutidos. Um patch com o mesmo nome de um embutido o substitui; arquivos de outros jogos (`TitleID`) ou que
não abrem são ignorados, com aviso no log. `isEnabled="true"` liga o patch com o mod.

**Resoluções:** um patch chamado `Resolution Patch - <largura>x<altura>` vira uma opção de **Render
resolution** no launcher e no menu do jogo (tecla Insert), depois das embutidas. Vale a mesma regra das
resoluções embutidas (a nota pede memória com `Requires setting DMEM to N MB`). Se o mod é desativado, a
resolução volta a `native`.

*English:* a mod can be dropped into the mods folder as a **.zip** (it is unpacked into a folder with the zip's name when the launcher or the game starts; `tools/zip_mod.py` packs a mod folder). A mod folder can also carry **game patches**: XML files in its `patches/` folder, in the format of
the built-in patches (signatures, `Pattern`/`At`). They apply while the mod is enabled, together with the
built-in ones (a patch with the same name replaces it). A patch named `Resolution Patch - <width>x<height>`
becomes a **Render resolution** choice in the launcher and the in-game menu (Insert).
