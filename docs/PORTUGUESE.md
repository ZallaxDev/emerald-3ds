# Pokémon Emerald em português do Brasil

A compilação em português (`--port-lang pt_br`) tem duas partes:

- **A interface do port**, toda em português: os rótulos da tela de baixo
  (MAPA, os atributos, as opções de 3D), as abas OPÇÕES, MELHORIAS e
  TRAPAÇAS com suas células, e as mensagens de erro do pacote de dados.
- **O texto do jogo**, traduzido por inteiro: os diálogos de todos os mapas,
  os treinadores, as batalhas, os menus, a POKéDEX, o PC, as lojas, o
  POKéNAV, os concursos, a FRONTEIRA DE BATALHA e a FALA FÁCIL.
  `python tools/localize_portuguese.py --tree build/upstream --status`
  mostra a cobertura.
- **Os gráficos com palavras** que o port redesenha a partir da ROM do
  jogador (`3ds_port/src/3ds_icons_ptbr.c`): os ícones de tipo, a janela de
  golpes, os títulos do resumo e o APERTE START.

O plano da tradução, as decisões e o andamento de cada fase estão no
[roadmap](ROADMAP-PTBR.md); os termos, no
[glossário](../tools/locales/pt_br/GLOSSARIO.md).

O jogo continua vindo da ROM americana/europeia (BPEE). Testada num 3DS.

## Compilar

Com os requisitos da [guia de desenvolvimento](DEVELOPMENT.md):

```sh
python tools/bootstrap.py --make --port-lang pt_br -j8
```

Ou, numa árvore já preparada, `make -C 3ds_port PORT_LANG=pt_br`. Trocar
`PORT_LANG` recompila todos os objetos (o Makefile compara os flags em
`build/config`). `--port-lang pt_br` não se combina com `--spanish-rom`.

## Como funciona

`3ds_port/include/3ds_locale.h` escolhe cada texto do port com
`CTR_TEXT(inglês, espanhol, francês, italiano, português, japonês)`, ou
`CTR_TEXT_PT(inglês, português)` para os que as outras interfaces ainda mostram
em inglês. Os rótulos da tela
de baixo passam por `Ascii()` (`3ds_bottom_ui.c`), que aceita as letras
acentuadas em UTF-8: á à â ã é ê í ó ô õ ú ç e as maiúsculas. As mensagens
de erro saem pelo console do libctru, que é ASCII: vão sem acentos.

## ã, õ, Ã e Õ

As fontes do jogo não têm essas letras. O patch
`patches/pokeemerald/0057-portuguese-tilde-letters.patch` dá a elas os
códigos livres 0x2F a 0x32 do charmap (`'Ã'`, `'Õ'`, `'ã'`, `'õ'`, e
`CHAR_A_TILDE`, `CHAR_O_TILDE`, `CHAR_a_TILDE`, `CHAR_o_TILDE`) e as larguras
de A/O/a/o nas cinco fontes latinas. Os desenhos não estão no repositório:
quando cada fonte é carregada do pacote, `3ds_compat.c` copia a letra e põe
sobre ela o til do ñ (ou do Ñ) da mesma fonte, isto é, as linhas que o ñ
pinta e o n deixa em branco, centradas na largura da letra. Assim cada fonte
ganha um til no seu próprio estilo, e o texto do jogo pode usar
`_("não")` depois do patch.

## A ordem das palavras nas batalhas

O inglês põe "Wild" e "Foe" antes do nome do POKéMON e "sharply" antes de
"rose". O patch `patches/pokeemerald/0058-portuguese-battle-word-order.patch`
acrescenta, sem mudar nada em inglês:

- `sText_WildPkmnSuffix` e `sText_FoePkmnSuffix`, vazios em inglês, que vêm
  depois do nome: em português o prefixo fica vazio e o sufixo é
  " selvagem" ou " inimigo" (ZIGZAGOON selvagem usou INVESTIDA!);
- `sText_StatSharplyRose` e `sText_StatHarshlyFell`, a mudança forte de um
  atributo numa frase só: "subiu muito!", "caiu muito!".

Do mesmo jeito, `patches/pokeemerald/0059-portuguese-category-word-order.patch`
monta a categoria com um prefixo e um sufixo (`sText_CategoryPrefix`,
vazio em inglês, e `sText_CategorySuffix`, " POKéMON"): em português,
POKéMON SEMENTE em vez de SEMENTE POKéMON.

E `patches/pokeemerald/0060-portuguese-type-names.patch` dá 9 bytes aos
nomes dos tipos (`TYPE_NAME_LENGTH`, para TERRESTRE), alarga a tabela de
"um golpe de FOGO" como o patch 0034 faz para o espanhol e põe na fonte
estreita um tipo largo demais para as caixas da busca da POKéDEX. Na
janela de golpes da batalha, `gText_MoveInterfaceType` ("TYPE/") fica
vazio em português, para o nome inteiro caber nos 64 px.

Limites a respeitar ao traduzir:

- Células de opção: até 17 caracteres; abas: até 12 (com "1/2").
- Linhas das mensagens de erro: até 40 caracteres.

## Traduzir o texto do jogo

Não existe Pokémon Emerald oficial em português: a tradução é deste projeto.
Ela fica em `tools/locales/pt_br/`, um arquivo por fonte do pret (os textos
de `data/text/birch_speech.inc` em
`tools/locales/pt_br/data/text/birch_speech.inc.txt`), e
`tools/localize_portuguese.py` a aplica na árvore do bootstrap antes da
compilação. Cada entrada é o rótulo do texto, o hash do texto inglês e o
texto em português, escrito como no pret:

```text
[gText_Birch_BoyOrGirl] 904f2ae23a
Você é um menino?\n
Ou é uma menina?$
```

As linhas da entrada se juntam como estão (uma linha em branco termina a
entrada); `\n` quebra a linha, `\l` rola o texto, `\p` abre um novo
parágrafo, e os textos dos `.inc` terminam em `$`. O inglês não é copiado
para cá: para traduzir um arquivo, veja seus textos e hashes com

```sh
python tools/localize_portuguese.py --tree build/upstream --show data/text/save.inc
```

Antes de mudar a árvore, a ferramenta confere tudo e não aplica nada se algo
falhar:

- o hash: se o texto inglês mudou, a tradução precisa ser revista;
- cada caractere existe no charmap (com ã, õ, Ã e Õ do patch 0057);
- os mesmos placeholders do inglês (`{PLAYER}`, `{STR_VAR_1}`...);
- o tamanho dos nomes de tamanho fixo (itens: 13 bytes; golpes e habilidades: 12; categorias: 11; tipos: 9);
- a largura de cada linha na fonte normal: até 208 px nos textos dos `.inc`
  (as caixas de mensagem têm 216), e nas strings de C, que costumam ser
  menus, a largura do inglês. `width=N` no cabeçalho permite N pixels, para
  uma entrada de menu que cabe na janela (`[gText_MenuBag] 332c000cf7
  width=42`: as entradas do menu START cabem na largura de POKéMON).

`--check` só valida.

### Convenções

A regra do projeto é traduzir o máximo possível com a **nomenclatura
oficial usada no Brasil**. Os termos oficiais, com a fonte, e as escolhas
para o que não tem nome oficial estão em
[`tools/locales/pt_br/GLOSSARIO.md`](../tools/locales/pt_br/GLOSSARIO.md):
confira lá antes de usar um termo novo.

- Nomes próprios em maiúsculas, como no jogo (POKéMON, BIRCH, MAY). Os
  nomes dos POKéMON não se traduzem. Nos lugares, o nome fica e a parte
  genérica se traduz: CIDADE DE LITTLEROOT, ROTA 103, GINÁSIO DE PETALBURG.
- O jogador pode ser menino ou menina: o texto que vale para os dois não
  flexiona gênero para ele (a MAMÃE diz “meu amor”, “meu bem”; “terá
  talento para treinar” em vez de “será um ótimo TREINADOR”). As falas de
  MAY só aparecem a um jogador e as de BRENDAN a uma jogadora, e podem
  flexionar.
- {RIVAL} é MAY ou BRENDAN: quem fala do rival sem saber qual evita o gênero
  (o PROF. BIRCH diz “{RIVAL}” ou “a minha cria”). O {STR_VAR_1} da mãe do
  rival é “nosso filho” ou “nossa filha” e fica no meio da frase.
- Placeholders que viram nomes de POKéMON ou itens ({STR_VAR_1}) vão sem
  artigo: “{PLAYER} recebeu {STR_VAR_1}!”.

## O pacote de dados

O texto do jogo faz parte dos dados que o builder monta a partir da ROM. Um
executável em português precisa de um pacote gerado para ele: a receita da
compilação em português leva os textos traduzidos como bytes literais (não
estão na ROM), e `tools/gen_recipe.py` recusa mais de 64 KiB de literais
(`--max-literal`), o que a tradução completa vai passar. Releases em
português e o builder para elas ainda estão por fazer; por enquanto, use os
dados soltos ou embutidos da compilação de desenvolvimento
([guia de desenvolvimento](DEVELOPMENT.md)).
