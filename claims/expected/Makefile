# Define the LaTeX compiler
PDFLATEX = pdflatex

# Define the BibTeX compiler
BIBTEX = bibtex

# The main source file of your document
SRC = permupd.tex

# Automatically determine the base name of the file (e.g., "permupd")
BASENAME = $(basename $(SRC))

# The final PDF file to be generated
PDF = $(BASENAME).pdf


# --- TARGETS ---

# The default target, executed when you just run "make"
.PHONY: all
all: $(PDF)

# The rule for creating the PDF
# This sequence is important:
# 1. pdflatex: Creates the .aux file with citation keys and references.
# 2. bibtex: Reads the .aux file and generates the bibliography (.bbl file).
# 3. pdflatex: Includes the bibliography into the document.
# 4. pdflatex: Ensures all cross-references (including citations) are correct.
$(PDF): $(SRC)
	$(PDFLATEX) $(BASENAME)
	$(BIBTEX) $(BASENAME)
	$(PDFLATEX) $(BASENAME)
	$(PDFLATEX) $(BASENAME)

# The target for cleaning up the directory
# Removes the final PDF and all intermediate files
.PHONY: clean
clean:
	rm -f $(PDF) *.aux *.bbl *.blg *.log *.out *.toc *.nav *.snm *.vrb
