from server.pipeline.sentences import SentenceSplitter


def split(*pieces: str) -> list[str]:
    splitter = SentenceSplitter()
    sentences = []
    for piece in pieces:
        sentences += splitter.feed(piece)
    return sentences + splitter.flush()


def test_sentences_are_cut_as_text_arrives():
    splitter = SentenceSplitter()
    assert splitter.feed("Paris. It is") == ["Paris."]
    assert splitter.feed(" on the Seine") == []
    assert splitter.flush() == ["It is on the Seine"]


def test_boundary_split_across_pieces():
    assert split("Yes", ".", " No", "!", " Maybe?") == ["Yes.", "No!", "Maybe?"]


def test_full_stop_at_the_end_waits_for_what_follows():
    splitter = SentenceSplitter()
    assert splitter.feed("It costs 3.") == []
    assert splitter.feed("5 dollars. Cheap.") == ["It costs 3.5 dollars."]
    assert splitter.flush() == ["Cheap."]


def test_abbreviations_and_initials_do_not_end_a_sentence():
    assert split("Dr. Smith met J. K. Rowling. They talked.") == [
        "Dr. Smith met J. K. Rowling.",
        "They talked.",
    ]


def test_dotted_forms_do_not_end_a_sentence():
    assert split("Come at 5 p.m. tomorrow. The U.S. economy grew.") == [
        "Come at 5 p.m. tomorrow.",
        "The U.S. economy grew.",
    ]


def test_punctuation_alone_is_never_a_sentence():
    assert split("Well. . . okay then. ") == ["Well.", ". . okay then."]
    assert split("Done. ...") == ["Done."]


def test_a_list_number_stays_with_its_item():
    assert split("1. First thing. 2. Second thing.") == [
        "1. First thing.",
        "2. Second thing.",
    ]


def test_closing_quote_stays_with_its_sentence():
    assert split('She said "go." Then left.') == ['She said "go."', "Then left."]


def test_line_break_ends_a_sentence():
    assert split("First line\nSecond line") == ["First line", "Second line"]


def test_empty_and_blank_input_gives_nothing():
    assert split("", "   ", "\n") == []
