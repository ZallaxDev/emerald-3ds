# Roadmap: Pokémon Emerald 3Ds Dual Screen em português (PT-BR)

Guia de trabalho da tradução PT-BR deste fork. Marque os itens à medida que
forem concluídos e registre as decisões na seção [Decisões](#decisões).

Fonte principal das traduções existentes: uma cópia dos dados da Gen 3 do
**HoennKantoWiki** em
[`tools/locales/pt_br/referencia/hoennkantowiki/`](../tools/locales/pt_br/referencia/hoennkantowiki/).
**O wiki não é alterado por este projeto:** só lemos e copiamos o que
precisamos.

Há um único fluxo de tradução: `python tools/bootstrap.py --make --port-lang pt_br`,
que aplica os catálogos de `tools/locales/pt_br/` com
`tools/localize_portuguese.py` (detalhes em [PORTUGUESE.md](PORTUGUESE.md)).

## Princípios

1. **Nenhum conteúdo da ROM no repositório.** A tradução é texto original
   nosso, aplicado ao código-fonte do pret. Gráficos e fontes continuam saindo
   da ROM do jogador, gerados localmente pelo builder.
2. **Traduzir o máximo.** Tudo o que der vai para o português. Quando uma
   palavra não couber, estudamos uma abreviação e a registramos no Glossário,
   em vez de manter o inglês.
3. **Fallback para o inglês.** Texto sem tradução continua em inglês. O jogo
   precisa compilar e rodar em qualquer estágio da tradução.
4. **Nomes traduzidos para soar natural.** Golpes, itens, habilidades,
   naturezas e a parte genérica dos lugares vão para o português
   (`ROUTE 104` vira `ROTA 104`, `MT. PYRE` vira `MONTE PYRE`). Ficam como
   estão os nomes de espécies de Pokémon (são os mesmos no Brasil), de
   personagens e o nome próprio dos lugares (`RUSTBORO`). Aqui o jogo diverge
   da regra do wiki, que mantém esses nomes em inglês.
5. **Fidelidade ao Emerald.** Textos com versão própria no Emerald (Pokédex,
   por exemplo) são traduzidos a partir do Emerald, não de Ruby/Sapphire.
6. **Save compatível.** `GAME_LANGUAGE` continua `LANGUAGE_ENGLISH`. O idioma da
   tradução é uma opção do port (`PORT_LANG=pt_br`), para que os saves sirvam
   nas duas versões. O pacote de dados do PT também sai da ROM BPEE, mas é
   próprio: os textos do jogo ficam no pacote, não no executável (ver fase 9).

## Visão geral

| Fase | Entrega | Depende de |
|---|---|---|
| 0 | Decisões de estilo e escopo | — |
| 1 | Infraestrutura de idioma (flag, catálogo, script, bootstrap) | 0 |
| 2 | Fonte com ã/õ | 1 |
| 3 | Ferramentas de qualidade (largura em pixels, cobertura) | 1 |
| 4 | Importação do wiki (golpes, categorias, tipos, locais) | 2, 3 |
| 5 | Nomes, textos curtos e Pokédex do Emerald | 4 |
| 6 | Interface, menus, batalha e ícones | 2, 3 |
| 7 | Diálogos e scripts do mundo | 3 |
| 8 | Tela de toque do 3DS | 1 |
| 9 | Builder, release e distribuição | 1, 2 |
| 10 | Teste completo e lançamento | todas |

As fases 5 a 8 podem andar em paralelo depois que 1 a 3 estiverem prontas.

---

## Fase 0 — Decisões

- [x] Caixa dos termos do jogo: estilo do Emerald (`POKéMON`, `ROTA 104`).
- [x] Nomes de tipos: os oficiais do Brasil, em maiúsculas (`FOGO`, `PLANTA`, `AÇO`).
- [x] Atributos: traduzidos (`ATAQUE`, `DEFESA`, `VELOCIDADE`…).
- [x] Tratamento ao jogador: "você" e tom informal.
- [x] Glossário inicial: [`tools/locales/pt_br/GLOSSARIO.md`](../tools/locales/pt_br/GLOSSARIO.md).
- [x] Nome do idioma no código: `pt_br`.

**Pronto quando:** as decisões estiverem registradas abaixo.

## Fase 1 — Infraestrutura de idioma

- [x] `PORT_LANG=pt_br` no Makefile do port; `GAME_LANGUAGE` continua inglês.
- [x] Catálogos em `tools/locales/pt_br/`, espelhando a árvore do pret: os
      textos de `data/text/birch_speech.inc` ficam em
      `tools/locales/pt_br/data/text/birch_speech.inc.txt`. Cada entrada nomeia
      o rótulo ou símbolo, o hash do texto em inglês e o texto em PT, escrito
      como no pret (`\n`, `\p`, `\l`, `{PLAYER}`).
- [x] `tools/localize_portuguese.py`:
  - [x] Substitui os textos de arquivos `.inc` e C pelo rótulo ou símbolo.
  - [x] Para se o texto em inglês mudou (hash diferente: upstream mudou).
  - [x] Confere caracteres da fonte e a largura em pixels de cada linha.
  - [x] `--check`, `--status` (progresso) e `--show` (hash, largura e inglês).
- [x] `tools/bootstrap.py --port-lang pt_br`: aplica os catálogos depois dos
      patches e antes do build; reinicia a árvore ao trocar de idioma.
- [x] Um texto traduzido aparece no jogo (conferido no 3DS de verdade, 08/10).

**Pronto quando:** `python tools/bootstrap.py --make --port-lang pt_br` gera um
3DSX (com os dados embutidos, como toda build de desenvolvimento) que mostra os
textos traduzidos.

## Fase 2 — Fonte com ã e õ

A tabela de caracteres da Gen 3 não tem `ã`, `õ`, `Ã` nem `Õ`.

- [x] Patch `0057`: `charmap.txt` mapeia `Ã`, `Õ`, `ã` e `õ` para os códigos
      livres `2F`–`32`.
- [x] O port desenha os glifos ao carregar as fontes (`3ds_compat.c`), a
      partir dos da ROM. Nada do gráfico é distribuído.
- [x] Teclado de nomes: sem `ã`/`õ` por enquanto (decidido em 12/10).
- [x] ã e õ conferidos no 3DS (08/10: "acentuação e til normais"), no lugar
      da captura no emulador.

**Pronto quando:** as quatro letras aparecem certas em todas as fontes no
emulador e no 3DS.

## Fase 3 — Ferramentas de qualidade

- [x] Checagem embutida em `tools/localize_portuguese.py` (não há um
      `ptbr_check.py` separado):
  - [x] Mede cada texto em **pixels**, com as larguras de `src/fonts.c`, e
        compara com a largura da janela onde ele aparece.
  - [ ] Confere o número de linhas por tipo de texto (descrição de golpe,
        Pokédex, caixa de diálogo de 2 linhas e `\p`).
  - [ ] Confere que os códigos de controle (`{PLAYER}`, `{STR_VAR_1}`,
        `{COLOR}`, `\n`, `\l`, `\p`, `$`) do original estão na tradução.
  - [x] Recusa caracteres fora da tabela.
  - [x] Relatório de cobertura por arquivo (`--status`) (traduzidos, faltando, com erro).
  - [ ] Lista de textos que não cabem, para estudar abreviações.
- [x] Quebra automática de linha: dispensada; os catálogos foram escritos à
      mão, com as quebras conferidas pela checagem de largura.
- [ ] Testes em `3ds_port/tests` ou `builder/tests` rodando o check sobre o
      catálogo.

**Pronto quando:** o check roda no catálogo inteiro e falha em erros reais.

## Fase 4 — Importação do HoennKantoWiki

- [x] Cópia dos dados do wiki em `tools/locales/pt_br/referencia/hoennkantowiki/`
      (só leitura do wiki).
- [x] Conversor da cópia do wiki: dispensado; os catálogos foram escritos à
      mão, com a cópia só como referência.

| Conteúdo | Arquivo da cópia | Destino no pret | Volume |
|---|---|---|---|
| Descrições de golpes | `i18n/pt.json` (`moves`) | `src/data/text/move_descriptions.h` | 354 |
| Categorias | `i18n/pt.json` (`genera`) | `categoryName` em `src/data/pokemon/pokedex_entries.h` | 279 (de 386) |
| Tipos | `i18n/pt.json` | `gTypeNames` | 18 |
| Descrições de itens | `item-descriptions.json` | `src/data/text/item_descriptions.h` | 18 (de ~310) |
| Termos de local | regras do wiki | `src/data/region_map/region_map_sections.json` | ~210 |

- [x] Golpes: ligar a chave do wiki (`thunderbolt`) ao símbolo
      (`sThunderboltDescription`), quebrar as linhas e listar o que não couber.
      O texto em PT ficou cerca de 22% maior que o inglês (mediana), e 232 de
      354 crescem mais de 15%.
- [x] Categorias: as 386, em até 11 bytes, com “POKéMON” antes
      (patch `0059`); quando não cabe, a palavra mais próxima (MIRMELEÃO,
      TATURANA, OTÁRIA).
- [x] Tipos: `TYPE_NAME_LENGTH` de 6 para 9 (`TERRESTRE`), com os nomes
      oficiais inteiros (patch `0060`). Na janela de golpes da batalha o
      "TIPO/" sai para o nome caber; na busca da Pokédex, um tipo largo usa a
      fonte estreita.
- [x] `sATypeMove_Table` em PT ("um golpe de FOGO"), alargando a tabela como o
      patch `0034` fez (`0060`, cortando a cópia no tamanho do buffer).
- [x] Estilo do jogo (caixa alta nos termos, tipos e atributos do Glossário):
      aplicado direto nos catálogos.
- [x] Locais: feitos na Fase 5 (opção a, 09/10): as cidades só com o nome
      próprio, ROTA 101, até 16 caracteres.

**Pronto quando:** o catálogo importado passa no check da fase 3 e as telas de
golpes e Pokédex aparecem em PT no jogo.

## Fase 5 — Nomes, textos curtos e Pokédex

Nomes primeiro, porque as descrições e os diálogos citam esses nomes.

- [x] Levantar referências de nomes em PT-BR (TCG, anime e dublagem, jogos e
      materiais oficiais recentes) e definir a fonte de cada categoria no
      Glossário.
- [x] **Golpes (354):** `src/data/text/move_names.h`, limite
      `MOVE_NAME_LENGTH` = 12.
- [x] **Itens (~370):** campo `name` em `src/data/items.h`, limite
      `ITEM_NAME_LENGTH` = 14.
- [x] **Habilidades (76):** `gAbilityNames` em `src/data/text/abilities.h`,
      limite `ABILITY_NAME_LENGTH` = 12.
- [x] **Bagas:** o nome fica no limite de 6 de `BERRY_NAME_LENGTH`, que faz
      parte do save; o "BERRY" que o código acrescenta virou FRUTA / FRUTAS,
      antes do nome (patch `0064`, 09/10).
- [x] **Lugares:** no mapa, na placa e no resumo, as cidades só com o nome
      próprio (LITTLEROOT) e o resto em até 16 caracteres (ROTA 101, CAT.
      METEOROS), pelo cabeçalho gerado `region_map_entries.h` (09/10).
- [x] Naturezas (25, nomes `pt_br` do Bulbapedia), classes de treinador (66),
      FITAS (66) e decorações da BASE SECRETA (120 nomes e 120 descrições)
      feitas em 09/10; as palavras da FALA FÁCIL (1008) também, com a ordem
      alfabética refeita do português (`tools/easy_chat_order.py`).
- [x] Limites de golpe, item e habilidade: mantidos; o que não coube foi
      abreviado e registrado no glossário (REPELENTE MÁX, PRATO CHUVA…).
- [x] **Habilidades (76):** o PT do wiki é a versão expandida (mediana de 2,2×
      o inglês, até 83 caracteres). No jogo cabe uma linha curta ("Ups GRASS
      moves in a pinch."). Escrever versões curtas, aproveitando o sentido do
      wiki.
- [x] **Pokédex (386):** o wiki tem as entradas de Ruby/Sapphire
      (`texto_historico` pega a primeira versão). Traduzir as entradas do
      **Emerald** a partir de `pokedex_text.h`, com o PT do wiki como apoio
      quando o texto for igual.
      Feito: as 386 e a de espécie desconhecida, em até 4 linhas e
      224 px, com medidas no sistema métrico. Conferir no 3DS as mais
      largas.
- [x] Descrições de itens.
- [x] Descrições das bagas e da etiqueta (`src/berry.c`), feitas no PC dos
      mapas em 10/10.

## Fase 6 — Interface, menus e batalha

Ordem sugerida, do que o jogador vê mais para o que vê menos:

- [x] Menus de `src/strings.c` (1205 de 1762 textos em 09/10): menu
      principal, CONTINUAR, opções (patches `0061` e `0062`), menu START,
      BOLSA, PC e CAIXAS, resumo, equipe, POKéDEX, POKéNAV, a caixa SIM/NÃO
      dos scripts, a janela de subida de nível; o resto (FRONTEIRA,
      concursos, PRESENTE MISTERIOSO, cartão de treinador, decorações,
      jogos sem fio, interface da FALA FÁCIL…) pelo PC dos mapas (Fase 7).
      Os que faltam não mudam (nomes, símbolos, números), não têm uso no
      código ou são o japonês do jogo original.
- [x] Arquivos comuns de `data/text` (feitos em 09/10: `save.inc`, `pc.inc`,
      `surf.inc`, `pc_transfer.inc` com a pergunta do apelido,
      `obtain_item.inc`). O resto passou ao PC dos mapas (Fase 7) em 09/10.
- [x] FRONTEIRA DE BATALHA: nomes das instalações decididos em 09/10 (ver o
      glossário: TORRE, CÚPULA, FÁBRICA, PALÁCIO, ARENA, PICO e PIRÂMIDE DE
      BATALHA; CÉREBRO DA FRONTEIRA; PONTOS DE BATALHA, PB).
- [x] Decorações nas listas de prêmios da FRONTEIRA: os bonecos, o pôster
      e a almofada KISS entraram no glossário com a CENTRAL DE TROCAS
      (09/10).
- [x] FRUTAS no singular e no plural: "FRUTA ORAN", "FRUTAS ORAN", com a
      palavra antes do nome, como no espanhol (patch `0064`, 09/10).
- [x] Janela de subida de nível na batalha (port, `3ds_video.c`, corrigido
      e conferido no 3DS em 09/10): ficava na
      cena ampliada 1,5×, atrás da caixa de mensagem, que esconde AT. ESP.,
      DEF. ESP. e VELOC. A versão original em inglês tem o mesmo corte
      (conferido no 3DS em 09/10): é um bug do port, não da tradução.
- [x] Ícones de tipo do RESUMO (`graphics/types/move_types.4bpp.lz`):
      redesenhados depois de descomprimir, sem comprimir de novo (patch
      `0066`, 10/10).
- [x] Ícones de condição: ENV, PAR, DOR, CON, QUE, DES (PKRS fica), no
      resumo (patch `0066`, que leva ao port toda folha descomprimida) e na tela de baixo, redesenhados pelo port
      (10/10).
- [x] Gráficos com palavras, vistos no 3DS em 10/10: o APERTE START da
      tela de título, o logo VERSÃO ESMERALDA (desenho próprio, reto,
      aprovado no 3DS em 12/10; desde 20/10 o ESMERALDA do logo espanhol,
      em arco, com VERSÃO no lugar de EDICIÓN, tabela `sLogo`), os
      títulos e os ícones de tipo do resumo (10/10) e as categorias de
      concurso (ESTILO, BELEZA, FOFURA, ESPERT., FORÇA, 11/10). Desde 11/10
      o patch `0066` leva ao port toda folha que o jogo descomprime.
- [ ] Conferir no 3DS os gráficos de 10 e 11/10, já pelo caminho do patch
      `0066`: títulos e ícones do resumo, ícones de condição e de concurso,
      APERTE START, POKéDEX (abas, menu START, busca, lista) e POKéNAV
      (opções, cabeçalhos, legenda do mapa, SAIR). Em teste pelo usuário.
- [x] Títulos dos concursos (resultado): NÍVEL NORMAL / SUPER / HIPER /
      MESTRE e as categorias, numa fonte alta desenhada como a original
      (12/10). Sem o patch `0032`: os textos cabem nos 40 px de cada parte.
      Conferir no 3DS com a lista acima.
- [ ] POKéDEX: na tela que registra um POKéMON novo, o texto aparece
      cortado (10/10). Conferir na versão original em inglês antes de
      investigar.
- [x] Menus da POKéDEX e do POKéNAV, escritos com as fontes do próprio
      jogo (`Port_LatinGlyph`), 11/10: na POKéDEX, as abas, o menu START,
      a busca e a lista; no POKéNAV, as opções, os cabeçalhos, a legenda do
      mapa ampliado e o SAIR da tela de condição. O cabeçalho POKéNAV /
      POKéMON NAVIGATOR fica (marca).
- [x] CENTRO POKéMON: na cura, as POKé BOLAS e o brilho da tela aparecem
      fora do lugar. Bug do port original (conferido na 0.3.1 em 10/10):
      fica para o autor do port corrigir, não neste fork.
- [ ] Golpes com animação na tela toda, como SURF: só cobrem a parte da tela
      que existia no GBA (240 px dos 400 do 3DS). Acontece também na versão
      original do port (conferido em 09/10): é uma limitação do port, não da
      tradução, nem da correção da janela de nível. Fica para depois,
      opcional.
- [x] Mensagens de batalha (`src/battle_message.c`, patch `0058` para a
      ordem “ZIGZAGOON selvagem”); conferir larguras com a
      referência do patch `0034`.
- [x] Resumo do POKéMON (natureza, origem, OVO), tela da equipe com as
      mensagens, telas da POKéDEX (busca, ordem, ALT./PESO) e unidades:
      metros e quilos com vírgula (patch `0063`, sobre o código do `0035`).
- [x] PC do jogador, sistema de CAIXAS, loja, opções e salvamento.
- [x] Concursos, FRONTEIRA DE BATALHA, COLINA DOS TREINADORES e BASES
      SECRETAS (textos de `src/strings.c` e de `data/text`, 09/10). Falta
      conferir no 3DS os títulos dos concursos (o espanhol precisou do patch
      `0032`).
- [x] "THE END" no fim dos créditos: fica em inglês, como os créditos
      (decidido em 12/10).
- [x] Ícones de tipo em PT: o port (não o builder) redesenha o nome sobre o
      ícone original, com uma fonte própria (`3ds_icons_ptbr.c`): BOLSA,
      janela de golpes, resumo e tela de baixo.
- [x] Atributos: são texto na janela de subida de nível e no resumo; os
      gráficos do resumo foram redesenhados pelo port (10/10), sem builder.
- [x] Atributos: larguras conferidas; no resumo, AT.ESP e DEF.ESP (centralizados
      em 36 px), na equipe e nas mensagens, AT. ESP. e DEF. ESP.

## Fase 7 — Diálogos e scripts do mundo

O maior volume do projeto, sem equivalente no wiki.

- [x] Inventário e cobertura: `localize_portuguese.py --status` conta por
      arquivo. Em 09/10, 8621 de 9178 textos de `data/text`, dos mapas e de
      `src/strings.c`; 11922 textos aplicados no total.
- [x] Traduzir **na ordem do jogo**, para cada bloco ser testável jogando:
  - [x] Littleroot, Oldale, Route 101–103, Petalburg (caminhão,
        LITTLEROOT inteira, ROTAS 101 a 103, OLDALE, PETALBURG inteira
        com o GINÁSIO)
  - [x] Rustboro, Dewford, Slateport (até o 3º ginásio): ROTA 104, BOSQUE
        DE PETALBURG, RUSTBORO, ROTA 116, TÚNEL RUSTURF, DEWFORD, CAVERNA
        GRANITO, ROTAS 105 a 109 e SLATEPORT inteira
  - [x] Mauville até Fortree (4º ao 6º ginásio): ROTA 110 com a CASA DOS
        TRUQUES, MAUVILLE, ROTAS 111 a 119, VERDANTURF, MONTE CHIMNEY,
        LAVARIDGE, FALLARBOR, CATARATA DOS METEOROS, ESCONDERIJO MAGMA,
        INSTITUTO DO CLIMA e FORTREE
  - [x] Lilycove, Mossdeep, Sootopolis, Team Magma/Aqua
        (feitos: ROTAS 120 e 121, MONTE PYRE, LILYCOVE inteira,
        ESCONDERIJO AQUA, ROTA 124, MOSSDEEP com o GINÁSIO e o CENTRO
        ESPACIAL, CAMINHO SUBMARINO, ROTA 128, SOOTOPOLIS com o GINÁSIO,
        CAVERNA DA ORIGEM, PILAR CELESTE, ESTRADA DA VITÓRIA, EVER
        GRANDE, a LIGA POKéMON, PACIFIDLOG, ROTA 123, NAVIO ABANDONADO,
        S.S. TIDAL, NOVA MAUVILLE, TORRE MIRAGEM e CAVERNA DO BANCO DE
        AREIA: todos os mapas da história estão feitos; as ROTAS 125 a 127 e 129 a 134
        não têm falas nos arquivos dos mapas)
  - [x] Ever Grande e Liga (ESTRADA DA VITÓRIA, EVER GRANDE e a LIGA
        POKéMON, 09/10)
  - [x] Pós-jogo e Battle Frontier
        (feita a FRONTEIRA DE BATALHA inteira em 09/10: portão, praças,
        salões, as sete instalações e a CENTRAL DE TROCAS; também a
        COLINA DOS TREINADORES e a ILHA DISTANTE: todos os arquivos
        data/maps com texto têm catálogo)
  - [x] Treinadores comuns das rotas (`data/text/trainers.inc`, 1.142
        textos) e a ZONA DE SAFÁRI (`data/scripts/safari_zone.inc`), 09/10
  - [x] Os outros `data/scripts` compartilhados (homem de MAUVILLE, salão
        de concursos, moça de LILYCOVE, LIQUIDIFICADOR, CENTRO DE CUIDADOS,
        BASES SECRETAS, árvores de FRUTA, golpes de campo, ROLETA e os
        PRESENTES MISTERIOSOS: 22 arquivos, cerca de 470 textos), 09/10
  - [x] O resto de `src/strings.c` (09/10, a pedido do usuário): todos os
        textos que o jogo usa, com a largura de cada janela conferida no
        código. Ficam de fora os ~370 sem referência no código e os que não
        mudam (nomes, símbolos, códigos de cor). As palavras da FALA FÁCIL
        seguem em inglês até a decisão da Fase 6; a interface dela já está
        traduzida.
  - [x] O resto de `data/text` (concursos, `cable_club`,
        `secret_base_trainers`, `trick_house_mechadolls`, `pokedex_rating`,
        `mauville_man`, `pokemon_news`, `questionnaire`, `abnormal_weather`),
        passado da Fase 6 e concluído em 09/10: todo o `data/text` tem catálogo
  - [x] `apprentice.inc` (288), `battle_dome.inc` (114) e
        `frontier_brain.inc` (28): a FRONTEIRA DE BATALHA, passados da Fase 6
        e concluídos em 09/10
  - [x] `match_call.inc` (317, as ligações no POKéNAV) e `tv.inc` (401,
        os programas de TV), passados da Fase 6 e concluídos em 09/10
  - [x] Os `data/text` que não estavam em nenhuma fase: TENDAS DE BATALHA,
        quem dá FRUTAS, tutores de golpes, CANTO DA LOTERIA, MESTRE DO
        LIQUIDIFICADOR, CAVERNA SHOAL, bilhetes de evento, móveis e
        mistura de registros (11 arquivos, 214 textos), 09/10
- [x] Textos comuns aos mapas: todo o `data/text` e os `data/scripts`
      compartilhados têm catálogo (09/10).
- [x] Frases de treinador (intro, derrota, revanche): `trainers.inc`, 09/10.
- [x] Cartas e FALA FÁCIL: traduzidas em 09/10 (as frases são guardadas como
      números, então save e troca não mudam), com a ordem alfabética refeita.
- [x] Braille das ruínas: fica em inglês (decidido em 09/10).
- [x] Créditos e cena final: ficam em inglês, com duas páginas novas logo
      depois do título (patch `0065`): Tradução PT-BR, Carlos Mozart e
      AllGenWiki; Port para Nintendo 3DS, Daniel Cazalla (09/10).
- [x] Rever os arquivos de código com texto que nenhum catálogo cobre
      (levantamento de 10/10 no PC dos menus). Nada deles estava no
      GitHub; catalogados no PC dos mapas em 10/10: `src/berry.c` (as 86
      linhas de descrição; os nomes ficam), `src/landmark.c`,
      `src/map_name_popup.c`, `src/berry_blender.c`,
      `src/data/union_room.h` e `src/data/text/match_call_messages.h`. Os
      teclados (`text_input_strings.c`, `naming_screen.c`) ficam, menos a
      tecla "outros" da FALA FÁCIL; o teclado de nomes não tem acentos.
      Conferir no 3DS: o mapa da POKéNAV, a placa da PIRÂMIDE e a janela
      de 7 tiles da SALA UNIÃO.

## Fase 8 — Tela de toque do 3DS

- [x] Generalizar `CTR_TEXT(english, spanish)` em `3ds_port/include/3ds_locale.h`
      para três idiomas.
- [x] Traduzir os rótulos da tela de baixo (menu, OPTION, VOXEL 3D, 3D ANGLE,
      3D ZOOM, cheats).
- [x] Mensagens do port (pacote incompatível, erro de dados, versão).

## Fase 9 — Builder, release e distribuição

- [ ] Variante `payload/pt/` com o executável PT e a receita da ROM BPEE.
- [ ] Receita com o texto PT: os textos do jogo vão para `gamedata.bin` e
      `scripts.bin`, que o builder recria da ROM. O texto traduzido não existe
      na ROM e entra como literal; hoje `tools/gen_recipe.py` limita literais
      de objetos do jogo a 4 KB (`--max-game-literal`). Criar uma classe
      "tradução" para esses bytes, com limite próprio, e conferir com
      `release_audit.py --rom` que nenhum trecho da ROM entrou junto.
- [ ] Builder (Windows e web): **escolha de idioma**. Hoje a variante sai da
      ROM; o PT usa a mesma ROM que o inglês.
- [ ] Verificação do pacote: o 3DSX PT aceita só o pacote PT (ABI própria),
      e o 3DSX inglês recusa o pacote PT.
- [ ] `tools/build_release.py` e `tools/release_audit.py`: incluir a variante e
      conferir que nenhum dado da ROM entrou.
- [ ] Documentação: `docs/PORTUGUESE.md` (feito: como compilar), seção no
      `README.md` e entrada no `CHANGELOG.md`.
- [ ] Créditos da tradução e nota em `AI_DISCLOSURE.md` se houver texto gerado
      ou revisado com IA.

## Fase 10 — Teste completo e lançamento

- [ ] Jogar do início à Liga no Azahar/Citra com o relatório de cobertura em
      100% nas áreas da história.
- [ ] Jogar no 3DS de verdade (mínimo: abertura, uma batalha de ginásio,
      concurso, PC, salvamento e carregamento).
- [ ] Save do inglês abre no PT e vice-versa.
- [ ] Release beta (`vX.Y.Z-ptbr-beta`), coleta de erros e release final.

---

## Decisões

Registre aqui cada decisão, com data.

| Data | Decisão |
|---|---|
| 2026-10-07 | Caixa: estilo do Emerald. Nomes e termos em maiúsculas (`POKéMON`, `POÇÃO`, `ROTA 104`); o conversor converte o texto copiado do wiki. |
| 2026-10-07 | Tipos traduzidos com `TIPOS_PT`, em maiúsculas. Exige aumentar `TYPE_NAME_LENGTH` (6 hoje; `TERRESTRE` tem 9) e gerar ícones de tipo em PT. |
| 2026-10-07 | Atributos traduzidos (`ATAQUE`, `DEFESA`, `VELOCIDADE`…), divergindo da regra do wiki. Formas exatas no Glossário. |
| 2026-10-07 | Tratamento: "você", tom informal. |
| 2026-10-07 | Nome do idioma no código: `pt_br`. |
| 2026-10-07 | Nomes de golpes, itens, habilidades, naturezas e a parte genérica dos lugares também são traduzidos, para soar natural ao jogador brasileiro. Espécies, personagens e o nome próprio dos lugares ficam como estão. |
| 2026-10-07 | O HoennKantoWiki não é alterado. Os dados são copiados para `tools/locales/pt_br/referencia/hoennkantowiki/`. |
| 2026-10-07 | Traduzir o máximo possível. Quando uma palavra não couber, estudar uma abreviação (registrada no Glossário) em vez de manter o inglês. |
| 2026-10-08 | **Nomenclatura oficial do Brasil** (jogos, anime, TCG; Bulbapedia `pt_br`) acima do wiki. Substitui as escolhas de 07/10 que divergem: BOLSA (não Mochila), POKé BOLA, PS (não HP), tipos PLANTA e AÇO (não GRAMA e METAL). Referência única: `GLOSSARIO.md`. |
| 2026-10-08 | Tipos com o nome oficial inteiro (patch `0060`, `TYPE_NAME_LENGTH` 9), como previsto em 07/10. Na janela de golpes da batalha o "TIPO/" sai para o nome caber; na busca da Pokédex, um tipo largo usa a fonte estreita. (As abreviações de 6 bytes, registradas antes neste dia, foram descartadas.) |
| 2026-10-08 | Categorias da POKéDEX com “POKéMON” antes (“POKéMON SEMENTE”), patch `0059`, em até 11 bytes. |
| 2026-10-08 | Um só fluxo PT-BR: `--port-lang pt_br` com `localize_portuguese.py` e os catálogos de `tools/locales/pt_br/`. O fluxo `--locale ptbr` (`localize_ptbr.py`, catálogo TOML) foi retirado; os textos dele já estavam no catálogo `pt_br`. |

## Glossário

O glossário do projeto é
[`tools/locales/pt_br/GLOSSARIO.md`](../tools/locales/pt_br/GLOSSARIO.md):
termos oficiais, escolhas sem nome oficial, regras de abreviação de itens,
golpes e habilidades e o estilo das mensagens de batalha. Os atributos, que
ele ainda não lista, seguem esta tabela:

| Inglês | Português | Abreviação |
|---|---|---|
| HP | PONTOS DE SAÚDE | PS |
| ATTACK / DEFENSE | ATAQUE / DEFESA | AT. / DEF. |
| SP. ATK / SP. DEF | ATAQUE ESP. / DEFESA ESP. | AT. ESP. / DEF. ESP. (sem espaço onde a janela é estreita: AT.ESP.) |
| SPEED | VELOCIDADE | VELOC. |
| ACCURACY / EVASIVENESS | PRECISÃO / EVASÃO | |

## Riscos

| Risco | Mitigação |
|---|---|
| Textos do jogo vivem no pacote, não no executável | Receita PT com literais de tradução (fase 9) |
| Atualização do upstream muda símbolos ou textos | Entradas com o hash do inglês; o `localize_portuguese.py` para e aponta o que mudou |
| Texto PT mais longo estoura janelas | Check em pixels (fase 3) e ajustes de layout no estilo dos patches `0031`–`0036` |
| Códigos reaproveitados para ã/õ aparecem em nomes de saves ingleses | Escolher letras sem uso real; documentar |
| Volume dos diálogos | Ordem do jogo, cobertura medida e fallback para o inglês |
| Divergência entre wiki e jogo | O wiki é a referência de termos; o jogo é a referência do texto original |
| Tipos e atributos em PT mexem em limites e gráficos | Tipos em 9 bytes (patch `0060`); gerar ícones e rótulos no builder a partir da ROM; testar as telas com os nomes mais longos (`TERRESTRE`, `VELOCIDADE`) |
