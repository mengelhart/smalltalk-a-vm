"ByteArray kernel methods.
 Primitives installed by bootstrap: at: (60), at:put: (61),
 basicAt: (60), basicAt:put: (61), basicSize (62).
 size inherited from ArrayedCollection (prim 53, byte-aware)."

!ByteArray methodsFor: 'printing'!
printString
    | result |
    result := '#['.
    1 to: self size do: [:i |
        i > 1 ifTrue: [result := result, ' '].
        result := result, (self at: i) printString].
    result := result, ']'.
    ^ result! !

!ByteArray methodsFor: 'converting'!
asString
    "Interpret bytes as character codes and return a String."
    | result sz |
    sz := self size.
    result := String new: sz.
    1 to: sz do: [:i |
        result at: i put: (Character value: (self at: i))].
    ^ result! !
