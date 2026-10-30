# Mods de arquivos — WipEout Omega Collection

A aba **Mods** ativa cada mod e define a ordem de carga. As mudanças valem no próximo
início; o último mod vence quando dois trocam o mesmo arquivo. Não há mesclagem de arquivos.
O dump original não é alterado: o runner monta uma vista temporária em `out/mod-game-*`.

## Como montar um mod

Cada pasta em `mods/` espelha caminhos relativos existentes no dump escolhido. Não presuma
formatos de arquivos de outro jogo. Este exemplo é **sintético**: substitua `pasta-do-dump`
e `arquivo.ext` pelos nomes reais do seu dump:

```text
mods/
  Meu mod/
    pasta-do-dump/arquivo.ext
    mod.json
    preview.png
```

Também são aceitos os wrappers `dvdroot_ps4/` e `app0/`, ou uma pasta externa adicional.
`eboot.bin`, `sce_module` e `sce_sys` são protegidos; links simbólicos são recusados.
Os nomes são comparados sem distinguir maiúsculas e minúsculas. Confira no log os totais
`replaced` e `added`: zero substituições pode indicar um caminho errado ou outra região.

`mod.json` e a imagem são opcionais:

```json
{"name": "Meu mod", "author": "Seu nome", "version": "1.0",
 "description": "O que o mod muda.", "url": "https://example.org/"}
```

Um ZIP colocado na pasta de mods é extraído ao abrir o launcher ou iniciar o jogo. Pastas
já existentes não são substituídas; caminhos perigosos e links no ZIP são recusados.
Extraia 7z/RAR antes. `python tools/zip_mod.py mods/MeuMod` empacota uma pasta.

```sh
WIPEOUT_OC_GAME_DIR=/caminho/CUSA05670 WIPEOUT_OC_MODS_DIR=/caminho/mods python run.py
WIPEOUT_OC_GAME_DIR=/caminho/CUSA05670 WIPEOUT_OC_MODS_ENABLED=0 python run.py
```

A ordem e as desativações ficam em `mods.json` no diretório de dados;
`WIPEOUT_OC_MODS_CONFIG` seleciona outro arquivo. Pastas novas entram ativadas no fim,
em ordem alfabética. A chave geral também desliga a pasta `<pasta-do-jogo>-mods/`.

## Mods com patches de código

Um mod pode conter somente `patches/meu-patch.xml`, no formato de
[WipEout_Omega_Collection.xml](../patches/WipEout_Omega_Collection.xml).
O XML do mod deve declarar o **TitleID real** do dump e seu **APP_VER real** (por exemplo,
CUSA05670 / 01.07). O tag `01.00` do XML embutido não é a versão real desse dump.
A validação ocorre antes de substituir um patch embutido de mesmo `Name`; uma cópia
privada dos metadados aceitos é ajustada para a mesclagem. O XML original fica intacto.

Um `Pattern` precisa encontrar exatamente um local válido. Referências de `<Line Target="...">`
para `<Target Name="...">`, de `<Target Line="...">` para uma linha e valores `{rel:...}`
precisam ser completos e sem ciclos; dependências
inválidas, ambíguas ou fora da imagem fazem o patch inteiro ser ignorado. Outros placeholders
não são aceitos. `Address` sem assinatura só é autorizado para a combinação exata
CUSA05670 / APP_VER 01.07 / SHA-256 do `eboot.bin`:

```text
3a52e910f6c450cc2f0c85ef994be5d8c951c326a362bcc4775f29e6ff7f644c
```

O hash é calculado novamente a partir do executável. Outras builds precisam de assinaturas
seguras. O log distingue patches ignorados de nomes realmente aplicados.
`isEnabled="true"` define a ativação inicial; a chave geral de mods também os desliga.
Um patch `Resolution Patch - <largura>x<altura>` pode adicionar uma resolução; consulte
[ultrawide](ultrawide.md). Isso não comprova sua qualidade ou desempenho no jogo.

## Patches de terceiros

A pasta `patches/` no diretório de dados (ou a escolhida no launcher) aceita XML shadPS4 /
GoldHEN para `eboot.bin`. Valem o TitleID e APP_VER reais e a mesma política de segurança
acima. Não use o hash de um executável de God of War III como autorização para WipEout.
Os patches externos entram depois dos embutidos. A seleção fica em `patches.json`:

```json
{"enabled": ["arquivo.xml/Nome do patch"], "disabled": ["arquivo.xml/Outro patch"]}
```

```sh
WIPEOUT_OC_PATCHES_DIR=/caminho WIPEOUT_OC_PATCHES_CONFIG=/caminho/patches.json python run.py
```

Tipos de dados aceitos incluem `bytes`, `bytes16/32/64`, `float32/64`, `utf8` e `utf16`.
Patches incompatíveis são ignorados inteiros e reportados. Não há garantia de suporte a
qualquer XML ou build.

## Controles e créditos

O mapeamento genérico de teclado/controle e os nomes dos botões continuam disponíveis.
A injeção herdada de prompts WAD e da imagem no menu de pausa de God of War III foi
removida; ela não é uma opção de WipEout.

Os desenhos e a ferramenta em `tools/controller_diagrams/` vieram de Kristian Andreev na
[PR #234](https://github.com/FelipePrado0/gow3_pc/pull/234), sob GPL-2.0-or-later.
São material de autoria, sem arquivos do jogo; suas dependências de desenho não são
necessárias para usar o launcher.
