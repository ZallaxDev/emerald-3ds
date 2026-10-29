<p align="center">
  <img src="https://i.imgur.com/9sTaHwy.png" alt="Pokémon Emerald 3Ds Dual Screen" width="480">
</p>

<h1 align="center">Pokémon Emerald 3Ds Dual Screen — PT-BR</h1>

<p align="center">
  <strong>Hoenn em português do Brasil, nas duas telas do 3DS.</strong><br>
  Tradução do port nativo para Nintendo 3DS, com a nomenclatura oficial brasileira.
</p>

<p align="center"><a href="README.md">English</a> · Português</p>

---

A compilação em português traduz o
[Pokémon Emerald 3Ds Dual Screen](https://github.com/ZallaxDev/pokeemerald-3Ds-dualscreen),
um port nativo do Pokémon Emerald para o Nintendo 3DS, para o **português do
Brasil**: o jogo roda na tela de cima, a tela de baixo é uma interface de
toque no lugar do menu START, e há um mundo em voxel opcional.

## O que está traduzido

- **Todo o texto do jogo:** diálogos de todos os mapas, treinadores,
  batalhas, menus, BOLSA, POKéDEX (as 386 entradas), resumo, PC, lojas,
  POKéNAV, concursos, FRONTEIRA DE BATALHA, FALA FÁCIL e mais de 14 mil
  textos ao todo.
- **Nomes oficiais do Brasil**, conferidos nos nomes `pt_br` do Bulbapedia:
  golpes, itens, habilidades, naturezas, tipos (FOGO, PLANTA, AÇO) e a parte
  genérica dos lugares (ROTA 101, CIDADE DE LITTLEROOT). Espécies,
  personagens e nomes próprios de lugares ficam como no original.
- **A interface do port:** a tela de baixo, as opções e as mensagens.
- **Gráficos:** os ícones de tipo e de condição, os títulos do resumo, os menus da POKéDEX e do POKéNAV, a janela de golpes,
  o logo VERSÃO ESMERALDA e o APERTE START da tela de título, redesenhados pelo port a partir da ROM do
  jogador.
- **Letras com til** (ã, õ, Ã, Õ), que as fontes do jogo não tinham, e as
  medidas da POKéDEX em metros e quilos.

O save é o mesmo do jogo em inglês.

## Problemas conhecidos

- **POKéDEX:** na tela que registra um POKéMON novo, o texto aparece cortado.
- Do port original, não da tradução: golpes com animação na tela toda (SURF)
  só cobrem os 240 px do GBA, e a animação da cura no CENTRO POKéMON aparece
  fora do lugar.

## Como jogar

Você precisa da **sua própria ROM** do Pokémon Emerald americano/europeu
(BPEE). Nenhum conteúdo da ROM está neste repositório nem nos releases.

Para compilar, com os requisitos do
[guia de desenvolvimento](docs/DEVELOPMENT.md):

```sh
python tools/bootstrap.py --make --port-lang pt_br
```

O resultado fica em `build/upstream/3ds_port/emerald3ds.3dsx`. Detalhes em
[docs/PORTUGUESE.md](docs/PORTUGUESE.md); instalação e som em
[docs/INSTALLATION.md](docs/INSTALLATION.md).

## Documentação da tradução

| Guia | Conteúdo |
| :--- | :--- |
| [Roadmap](docs/ROADMAP-PTBR.md) | Fases, decisões e o que falta. |
| [Glossário](tools/locales/pt_br/GLOSSARIO.md) | Os termos usados e suas fontes. |
| [Compilação em português](docs/PORTUGUESE.md) | Como compilar e como a tradução funciona. |
| [Changelog](CHANGELOG.md) | As mudanças de cada versão. |

## Créditos

- **Tradução PT-BR:** Carlos Mozart (AllGenWiki).
- **Port para Nintendo 3DS:** Daniel Cazalla
  ([ZallaxDev](https://github.com/ZallaxDev/pokeemerald-3Ds-dualscreen)).
- **Decompilação:** [pret/pokeemerald](https://github.com/pret/pokeemerald).

Pokémon é marca de Nintendo, Creatures e GAME FREAK. Este é um projeto de fã,
sem fins lucrativos e sem ligação com essas empresas. As licenças estão em
[LICENSE-PORT.md](LICENSE-PORT.md) e [NOTICE.md](NOTICE.md).
