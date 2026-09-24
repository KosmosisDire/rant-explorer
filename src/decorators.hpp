/* Decorators RCSS lacks, drawn in the element's text colour:
   dashed-ring(n width) is a circle of n dashes inside the edge of the border box, and
   dotted-underline(size gap) a row of square dots along the bottom of the content box. */
#ifndef DECORATORS_HPP
#define DECORATORS_HPP

/* Registers both. Call after Rml::Initialise and before any document loads. */
void decorators_init();

#endif /* DECORATORS_HPP */
