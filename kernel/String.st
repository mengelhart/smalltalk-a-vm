"String — byte-indexable sequence of characters.
 size inherited from ArrayedCollection (prim 53, byte-aware).
 at: (prim 63) returns Character, at:put: (prim 64) takes Character.
 Installed by bootstrap."

!String methodsFor: 'copying'!
, aString
    "Concatenation. Return a new String."
    | newSize result i |
    newSize := self size + aString size.
    result := String new: newSize.
    i := 1.
    [i <= self size] whileTrue: [
        result at: i put: (self at: i).
        i := i + 1].
    [i <= newSize] whileTrue: [
        result at: i put: (aString at: i - self size).
        i := i + 1].
    ^ result!
copyFrom: start to: stop
    "Return a substring from start to stop (1-based, inclusive)."
    | newSize result i |
    newSize := stop - start + 1.
    newSize < 0 ifTrue: [newSize := 0].
    result := String new: newSize.
    i := 1.
    [i <= newSize] whileTrue: [
        result at: i put: (self at: start + i - 1).
        i := i + 1].
    ^ result! !

!String methodsFor: 'converting'!
reversed
    | sz result i |
    sz := self size.
    result := String new: sz.
    i := 1.
    [i <= sz] whileTrue: [
        result at: i put: (self at: sz - i + 1).
        i := i + 1].
    ^ result!
asUppercase
    | sz result i |
    sz := self size.
    result := String new: sz.
    i := 1.
    [i <= sz] whileTrue: [
        result at: i put: (self at: i) asUppercase.
        i := i + 1].
    ^ result!
asLowercase
    | sz result i |
    sz := self size.
    result := String new: sz.
    i := 1.
    [i <= sz] whileTrue: [
        result at: i put: (self at: i) asLowercase.
        i := i + 1].
    ^ result! !

!String methodsFor: 'comparing'!
= aString
    "Character-by-character comparison."
    | sz i result |
    result := false.
    self == aString
        ifTrue: [result := true]
        ifFalse: [
            (aString respondsTo: #size)
                ifTrue: [
                    sz := self size.
                    sz = aString size
                        ifTrue: [
                            result := true.
                            i := 1.
                            [i <= sz and: [result]] whileTrue: [
                                (self at: i) = (aString at: i)
                                    ifFalse: [result := false].
                                i := i + 1]]]].
    ^ result!
< aString
    "Lexicographic comparison."
    | minSz i result done |
    minSz := self size min: aString size.
    i := 1.
    result := false.
    done := false.
    [i <= minSz and: [done not]] whileTrue: [
        (self at: i) < (aString at: i)
            ifTrue: [result := true. done := true].
        done ifFalse: [
            (self at: i) > (aString at: i)
                ifTrue: [done := true]].
        i := i + 1].
    done ifFalse: [result := self size < aString size].
    ^ result! !

!String methodsFor: 'testing'!
isEmpty
    ^ self size = 0!
includes: aCharacter
    | sz i result |
    sz := self size.
    i := 1.
    result := false.
    [i <= sz and: [result not]] whileTrue: [
        (self at: i) = aCharacter ifTrue: [result := true].
        i := i + 1].
    ^ result! !

!String methodsFor: 'hashing'!
hash
    "Content-based hash. Uses whileTrue: to avoid mutable outer temp capture.
     Mask to 29-bit positive SmallInteger range (536870911 = 16r1FFFFFFF)."
    | h i sz |
    h := 0. i := 1. sz := self size.
    [i <= sz] whileTrue: [
        h := (h * 31 + (self at: i) value) bitAnd: 536870911.
        i := i + 1].
    ^ h! !

!String methodsFor: 'printing'!
printString
    "Return string representation with quotes.
     Smalltalk-80: 'hi' printString => '''hi'''"
    ^ '''''', self, ''''''! !
