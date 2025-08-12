#let datasheet(
  metadata: (
    organization: [Vowstar],
    logo: "./image/logo.svg",
    website_url: "https://github.com/vowstar/qsoc",
    title: [Document Title],
    product: [Product Name],
    product_url: "https://github.com/vowstar/qsoc",
    revision: [Revision Number],
    publish_date: [Publish Date],
  ),
  cover_image: "./image/cover.svg",
  features: [],
  applications: [],
  description: [],
  document: [],
  backcover: [],
) = {
  let fonts = (
    serif: ("Sarasa Mono SC", "Noto Serif"),
    sans: ("Sarasa Mono SC", "Noto Sans"),
    mono: ("Sarasa Mono SC", "Noto Sans Mono"),
    text: ("Sarasa Mono SC", "Noto Sans"),
    text_strong: ("Sarasa Mono SC", "Noto Sans"),
    headings: ("Sarasa Mono SC", "Noto Sans"),
    code: ("Sarasa Mono SC", "Noto Sans Mono"),
  )

  set text(font: fonts.text, size: 11pt)
  show strong: it => text(font: fonts.text_strong, it)
  show link: it => text(fill: rgb("#0000FF"))[#it]
  set page(paper: "a4")

  // Figure styles
  show figure.caption: set text(weight: "semibold", font: fonts.headings)

  // Table styles
  show figure.where(kind: table): set figure.caption(position: top)

  set table(
    stroke: 0.5pt,
    fill: (_, y) => if y == 0 { gray.lighten(75%) },
    align: (_, y) => if y == 0 { align(center) },
  )
  show table.header: strong
  show table.cell.where(y: 0): set text(weight: "semibold")

  let header_layout = () => {
    [
      #set text(10pt)
      #context {
        if calc.odd(here().page()) {
          grid(
            columns: (1fr, 1fr),
            rows: 100%,
            gutter: 3pt,
            [
              #set align(left)
              #link(metadata.website_url)[#image(metadata.logo, height: 28pt)]
            ],
            [
              #set align(right)
              #link(metadata.product_url)[#metadata.product]
              #linebreak()
              #metadata.revision - #metadata.publish_date
            ],
          )
        } else {
          grid(
            columns: (1fr, 1fr),
            rows: 100%,
            gutter: 3pt,
            [
              #set align(left)
              #link(metadata.product_url)[#metadata.product]
              #linebreak()
              #metadata.revision - #metadata.publish_date
            ],
            [
              #set align(right)
              #link(metadata.website_url)[#image(metadata.logo, height: 28pt)]
            ],
          )
        }
      }
      #v(-0.65em)
      #line(length: 100%, stroke: 1pt)
    ]
  }

  let footer_layout = () => {
    line(length: 100%, stroke: 1pt)
    v(-0.65em)
    set text(10pt, baseline: 0pt)
    context {
      if calc.odd(here().page()) {
        grid(
          columns: (5fr, 1fr),
          rows: auto,
          gutter: 0pt,
          [
            #set align(left)
            Copyright © #link(metadata.website_url)[#metadata.organization]
          ],
          [
            #set align(right)
            #counter(page).display("1 / 1", both: true)
          ],
        )
      } else {
        grid(
          columns: (1fr, 5fr),
          rows: auto,
          gutter: 0pt,
          [
            #set align(left)
            #counter(page).display("1 / 1", both: true)
          ],
          [
            #set align(right)
            Copyright © #link(metadata.website_url)[#metadata.organization]
          ],
        )
      }
    }
  }

  set par(leading: 0.75em)

  set page(
    numbering: "(1 / 1)",
    footer-descent: 2em,
    header: header_layout(),
    footer: footer_layout(),
  )

  set heading(numbering: "1.")

  show heading: it => block([
    #v(0.3em)
    #text(weight: "bold", font: fonts.headings, [#counter(
        heading,
      ).display() #it.body])
    #v(0.8em)
  ])

  show heading.where(level: 1): it => {
    block([
      #text(weight: "bold", font: fonts.headings, [#counter(
          heading,
        ).display() #it.body])
      #v(0.3em)
    ])
  }

  let render_cover_page = () => {
    set page(numbering: none, footer: none, header: none, margin: 0cm)
    set heading(numbering: none)
    image(cover_image, width: 100%, height: 100%)
  }

  let render_overview_page = () => {
    v(-0.65em)
    align(center, block({
      set text(16pt, font: fonts.headings, weight: "medium")
      metadata.title
      v(-0.5em)
      line(length: 100%, stroke: 1pt)
      v(0.3em)
    }))
    columns(2, gutter: 30pt)[
      = Features
      <TitlePageFeatures>
      #features

      = Applications
      <TitlePageApplications>
      #applications

      = Description
      <Description>
      #description
    ]
  }

  let render_toc_page = () => {
    set heading(numbering: none)
    show heading: it => block([#it.body])
    [
      #block([
        #columns(1, gutter: 30pt)[
          = Contents
          <Directory>
          #outline(title: none, depth: 3)
        ]
      ])
    ]
  }

  let render_indexing_page = () => {
    set page(numbering: none)
    set heading(numbering: none)
    [
      = Indexing
      #columns(2, gutter: 30pt)[
        == Figures
        #outline(title: none, target: figure.where(kind: image))

        == Tables
        #outline(title: none, target: figure.where(kind: table))
      ]
    ]
  }

  let render_backcover_page = () => [
    #counter(page).update(n => n - 1)
    #set page(numbering: none, header: none, footer: none)
    #show heading: it => it.body

    #v(2.5cm)

    #backcover
  ]

  // Main document flow
  render_cover_page()
  counter(page).update(1)
  counter(heading).update(0)
  pagebreak()

  counter(heading).update(0)
  render_overview_page()
  pagebreak()

  counter(heading).update(0)
  render_toc_page()
  pagebreak()

  counter(heading).update(0)
  document

  pagebreak()
  render_indexing_page()

  pagebreak()
  render_backcover_page()
}
