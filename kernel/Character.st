"Character kernel methods.
 Character is an immediate type (tagged OOP, not heap object).
 Primitive methods installed by bootstrap: value (prim 94).
 Class-side: value: (prim 95) on Character class."

!Character methodsFor: 'comparing'!
= aCharacter
    ^ self value = aCharacter value!
< aCharacter
    ^ self value < aCharacter value!
> aCharacter
    ^ self value > aCharacter value!
hash
    ^ self value! !

!Character methodsFor: 'testing'!
isLetter
    | v result |
    v := self value.
    result := false.
    (v >= 65 and: [v <= 90]) ifTrue: [result := true].
    (v >= 97 and: [v <= 122]) ifTrue: [result := true].
    ^ result!
isDigit
    | v |
    v := self value.
    ^ v >= 48 and: [v <= 57]!
isUppercase
    | v |
    v := self value.
    ^ v >= 65 and: [v <= 90]!
isLowercase
    | v |
    v := self value.
    ^ v >= 97 and: [v <= 122]!
isAlphaNumeric
    | result |
    self isLetter ifTrue: [result := true] ifFalse: [result := self isDigit].
    ^ result!
isSeparator
    "Space, tab, cr, lf, ff."
    | v result |
    v := self value.
    result := false.
    v = 32 ifTrue: [result := true].
    v = 9 ifTrue: [result := true].
    v = 10 ifTrue: [result := true].
    v = 13 ifTrue: [result := true].
    v = 12 ifTrue: [result := true].
    ^ result! !

!Character methodsFor: 'converting'!
asUppercase
    | v result |
    v := self value.
    result := self.
    (v >= 97 and: [v <= 122])
        ifTrue: [result := Character value: v - 32].
    ^ result!
asLowercase
    | v result |
    v := self value.
    result := self.
    (v >= 65 and: [v <= 90])
        ifTrue: [result := Character value: v + 32].
    ^ result!
asInteger
    ^ self value!
asString
    "Return a one-character String."
    | s |
    s := String new: 1.
    s at: 1 put: self.
    ^ s! !

!Character methodsFor: 'printing'!
printString
    | s |
    s := String new: 2.
    s at: 1 put: $$.
    s at: 2 put: self.
    ^ s! !
